import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components.esp32 import get_esp32_variant
from esphome.components.esp32.const import (
    VARIANT_ESP32C2,
    VARIANT_ESP32C3,
    VARIANT_ESP32C5,
    VARIANT_ESP32C6,
    VARIANT_ESP32C61,
    VARIANT_ESP32H2,
    VARIANT_ESP32H4,
    VARIANT_ESP32H21,
    VARIANT_ESP32S2,
)
from esphome.const import CONF_ID, CONF_MODE, CONF_RX_PIN, CONF_TIMEOUT, CONF_TX_PIN

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@jsimonetti"]

# Named "atlantic_v5_component", not "atlantic_v5": ESPHome's generated
# main.cpp does `using namespace esphome;`, so any *unqualified* reference to
# an esphome::atlantic_v5::* symbol would be ambiguous with the L1/L2 core's
# own global `namespace atlantic_v5 { ... }` (core/transport headers this
# component #includes) - ESPHome's own codegen never fully-qualifies with
# `esphome::`, so this collision isn't avoidable by qualifying our own calls
# alone. See build-and-tooling notes.
atlantic_v5_ns = cg.esphome_ns.namespace("atlantic_v5_component")
AtlanticV5Component = atlantic_v5_ns.class_("AtlanticV5Component", cg.Component)
Mode = atlantic_v5_ns.enum("Mode", is_class=True)
# Shared with sensor.py/binary_sensor.py/text_sensor.py: which publish_state()
# overload set_entity() should dispatch to.
EntityKind = atlantic_v5_ns.enum("EntityKind", is_class=True)

MODE_LISTENER = "listener"
MODE_MITM = "mitm"
MODES = {
    MODE_LISTENER: Mode.LISTENER,
    MODE_MITM: Mode.MITM,
}

CONF_HMI = "hmi"
CONF_MAIN = "main"
CONF_UART_NUM = "uart_num"
CONF_BUS_CAPTURE = "bus_capture"
CONF_TX_ENABLE_PIN = "tx_enable_pin"
CONF_ONE_WIRE_MIRROR = "one_wire_mirror"
CONF_RELAY_CORE = "relay_core"
CONF_FORWARD_BAD_CRC = "forward_bad_crc"
CONF_FRAME_SILENCE = "frame_silence"
CONF_ECHO_DRAIN = "echo_drain"
CONF_DIR_SETUP = "dir_setup"
CONF_DIR_HOLD = "dir_hold"
# Referenced by the read-only-entity platform files (sensor.py etc.) to look up
# this hub instance.
CONF_ATLANTIC_V5_ID = "atlantic_v5_id"

# ESP32-C3/S2 only have one UART port free besides the console, so MITM is
# unsupported there and rejected at validation time.
MITM_UNSUPPORTED_VARIANTS = {VARIANT_ESP32C3, VARIANT_ESP32S2}
# Single-core targets: pinning to a specific core (relay_core)
# doesn't apply. xTaskCreatePinnedToCore still works with tskNO_AFFINITY, which
# is what an unset relay_core resolves to on these variants (see to_code).
SINGLE_CORE_VARIANTS = {
    VARIANT_ESP32C2,
    VARIANT_ESP32C3,
    VARIANT_ESP32C5,
    VARIANT_ESP32C6,
    VARIANT_ESP32C61,
    VARIANT_ESP32H2,
    VARIANT_ESP32H4,
    VARIANT_ESP32H21,
    VARIANT_ESP32S2,
}

# The four bus timing defaults, duplicated from C++ so they show up
# in the resolved YAML. Keep in step with FrameAssembler::DEFAULT_SILENCE_US
# (assembler.h), DEFAULT_ECHO_DRAIN_US (relay.h), and DEFAULT_DIR_SETUP_US /
# DEFAULT_DIR_HOLD_US (uart_bus_io.h). dir_setup/dir_hold carry no schema
# default, so that setting either one in mode: listener can be rejected rather
# than silently ignored; to_code supplies the fallback instead.
DEFAULT_FRAME_SILENCE_US = 4000
DEFAULT_ECHO_DRAIN_US = 200
DEFAULT_DIR_SETUP_US = 10
DEFAULT_DIR_HOLD_US = 0

SIDE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_UART_NUM): cv.int_,
        cv.Required(CONF_RX_PIN): pins.internal_gpio_input_pin_number,
        cv.Optional(CONF_TX_PIN): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_TX_ENABLE_PIN): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_ONE_WIRE_MIRROR, default=False): cv.boolean,
        cv.Optional(CONF_DIR_SETUP): cv.positive_time_period_microseconds,
        cv.Optional(CONF_DIR_HOLD): cv.positive_time_period_microseconds,
    }
)


def _validate_sides(config):
    # config[CONF_MODE] is the raw string key (cv.enum keeps the enum value
    # separately as .enum_value); comparing against MODES[...] compares a str
    # to a codegen MockObj, whose __eq__ builds a C++ expression that is
    # always truthy, so this branch would fire unconditionally.
    has_hmi = CONF_HMI in config
    has_main = CONF_MAIN in config

    if config[CONF_MODE] == MODE_MITM:
        if not (has_hmi and has_main):
            raise cv.Invalid("mode: mitm requires both 'hmi:' and 'main:'")
        for key in (CONF_HMI, CONF_MAIN):
            if CONF_TX_PIN not in config[key]:
                raise cv.Invalid(f"'{key}: tx_pin' is required in mode: mitm", path=[key])
            # The mirror drives the RX pin during transmit and parks both pins as
            # inputs otherwise, which is only coherent if something switches the
            # transceiver's direction. With no DIR line it is parked in receive and
            # driving those pins back, so the mirror would contend with it.
            if config[key][CONF_ONE_WIRE_MIRROR] and CONF_TX_ENABLE_PIN not in config[key]:
                raise cv.Invalid(
                    "'one_wire_mirror' requires 'tx_enable_pin' on the same side; no known "
                    "board wants the mirror without a direction pin",
                    path=[key, CONF_ONE_WIRE_MIRROR],
                )
        if CONF_TX_ENABLE_PIN not in config[CONF_HMI] and CONF_TX_ENABLE_PIN not in config[CONF_MAIN]:
            _LOGGER.warning(
                "mode: mitm with neither side's tx_enable_pin set: transmission may not reach the "
                "bus unless the transceiver can always drive it, or the bus is open-drain. Watch "
                "the per-side frame counters once running."
            )
        return config

    if has_hmi == has_main:  # neither, or both
        raise cv.Invalid("mode: listener requires exactly one of 'hmi:' or 'main:'")
    side_key = CONF_HMI if has_hmi else CONF_MAIN
    # Not merely inert: on a board whose transceiver is parked in receive, that
    # transceiver already drives the TX pin, so handing the pin to the UART as
    # well puts two outputs on one net.
    if CONF_TX_PIN in config[side_key]:
        raise cv.Invalid(
            "'tx_pin' is not allowed in mode: listener, which never drives the bus",
            path=[side_key, CONF_TX_PIN],
        )
    # The DIR knobs only ever reach UartBusIo, which mode: listener never
    # builds - accepting them here would be accepting a setting that does
    # nothing.
    for key in (CONF_DIR_SETUP, CONF_DIR_HOLD):
        if key in config[side_key]:
            raise cv.Invalid(f"'{key}' only applies in mode: mitm", path=[side_key, key])
    # Same reasoning, one level up: listener never forwards anything.
    if CONF_FORWARD_BAD_CRC in config:
        raise cv.Invalid(
            f"'{CONF_FORWARD_BAD_CRC}' only applies in mode: mitm", path=[CONF_FORWARD_BAD_CRC]
        )
    return config


def _final_validate(config):
    if config[CONF_MODE] != MODE_MITM:
        return
    variant = get_esp32_variant()
    if variant in MITM_UNSUPPORTED_VARIANTS:
        raise cv.Invalid(f"mode: mitm requires two free UART ports; not supported on {variant}")
    if CONF_RELAY_CORE in config and variant in SINGLE_CORE_VARIANTS:
        raise cv.Invalid(f"relay_core is not applicable on the single-core {variant}", path=[CONF_RELAY_CORE])


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AtlanticV5Component),
            cv.Optional(CONF_MODE, default=MODE_LISTENER): cv.enum(MODES, lower=True),
            cv.Optional(CONF_BUS_CAPTURE, default=False): cv.boolean,
            # If no valid frame has been seen for timeout (default 60s),
            # publish NAN and mark the component failed, gated on
            # MAIN specifically, not on HMI traffic.
            cv.Optional(CONF_TIMEOUT, default="60s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_HMI): SIDE_SCHEMA,
            cv.Optional(CONF_MAIN): SIDE_SCHEMA,
            # No default here: absence (vs. an explicit value) is meaningful,
            # see to_code and _final_validate.
            cv.Optional(CONF_RELAY_CORE): cv.int_range(min=0, max=1),
            # No schema default, like dir_setup/dir_hold: absence has to stay
            # distinguishable from an explicit False so mode: listener can
            # reject it rather than silently ignore it.
            cv.Optional(CONF_FORWARD_BAD_CRC): cv.boolean,
            # frame_silence must be non-zero: at zero the backstop
            # fires on every tick and no frame ever assembles.
            cv.Optional(CONF_FRAME_SILENCE, default=f"{DEFAULT_FRAME_SILENCE_US}us"): cv.All(
                cv.positive_not_null_time_period, cv.positive_time_period_microseconds
            ),
            cv.Optional(
                CONF_ECHO_DRAIN, default=f"{DEFAULT_ECHO_DRAIN_US}us"
            ): cv.positive_time_period_microseconds,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_sides,
)

FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_mode(config[CONF_MODE]))
    cg.add(var.set_bus_capture(config[CONF_BUS_CAPTURE]))
    cg.add(var.set_timeout(config[CONF_TIMEOUT]))
    cg.add(var.set_frame_silence(config[CONF_FRAME_SILENCE]))
    cg.add(var.set_echo_drain(config[CONF_ECHO_DRAIN]))

    if config[CONF_MODE] == MODE_MITM:
        for key, setter in ((CONF_HMI, var.set_hmi_uart), (CONF_MAIN, var.set_main_uart)):
            side = config[key]
            setter_args = [
                side[CONF_UART_NUM],
                side[CONF_RX_PIN],
                side[CONF_TX_PIN],
                side.get(CONF_TX_ENABLE_PIN, -1),
                side[CONF_ONE_WIRE_MIRROR],
                side.get(CONF_DIR_SETUP, DEFAULT_DIR_SETUP_US),
                side.get(CONF_DIR_HOLD, DEFAULT_DIR_HOLD_US),
            ]
            cg.add(setter(*setter_args))

        # Core 1 on dual-core targets, tskNO_AFFINITY otherwise — tskNO_AFFINITY
        # is passed through as -1 (see RelayTask::begin()).
        relay_core = config.get(CONF_RELAY_CORE)
        if relay_core is None:
            relay_core = -1 if get_esp32_variant() in SINGLE_CORE_VARIANTS else 1
        cg.add(var.set_relay_core(relay_core))
        cg.add(var.set_forward_bad_crc(config.get(CONF_FORWARD_BAD_CRC, False)))
        return

    side = config.get(CONF_HMI, config.get(CONF_MAIN))
    cg.add(var.set_uart_num(side[CONF_UART_NUM]))
    cg.add(var.set_rx_pin(side[CONF_RX_PIN]))
