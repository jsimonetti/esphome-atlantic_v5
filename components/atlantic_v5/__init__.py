import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_ID, CONF_MODE, CONF_RX_PIN, CONF_TIMEOUT, CONF_TX_PIN

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
# overload set_entity() should dispatch to (plan 3.7.1/3.7.3).
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
# Referenced by the read-only-entity platform files (sensor.py etc.) to look up
# this hub instance (plan 3.7.3's "cv.GenerateID(CONF_atlantic_v5_ID)").
CONF_ATLANTIC_V5_ID = "atlantic_v5_id"

# M0.5: only the pins actually consumed by listener-mode bus_capture (3.5.5).
# tx_enable_pin / one_wire_mirror are not accepted yet: they land with M6, once
# mitm transport exists to consume them.
SIDE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_UART_NUM): cv.int_,
        cv.Required(CONF_RX_PIN): pins.internal_gpio_input_pin_number,
        cv.Optional(CONF_TX_PIN): pins.internal_gpio_output_pin_number,
    }
)


def _validate_sides(config):
    # config[CONF_MODE] is the raw string key (cv.enum keeps the enum value
    # separately as .enum_value); comparing against MODES[...] compares a str
    # to a codegen MockObj, whose __eq__ builds a C++ expression that is
    # always truthy, so this branch would fire unconditionally.
    if config[CONF_MODE] == MODE_MITM:
        # MITM transport (3.5, 3.6) isn't built yet; don't accept a config that
        # would silently do nothing.
        raise cv.Invalid("mode: mitm is not implemented yet (lands at M6); use mode: listener")
    has_hmi = CONF_HMI in config
    has_main = CONF_MAIN in config
    if has_hmi == has_main:  # neither, or both
        raise cv.Invalid("mode: listener requires exactly one of 'hmi:' or 'main:'")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AtlanticV5Component),
            cv.Optional(CONF_MODE, default=MODE_LISTENER): cv.enum(MODES, lower=True),
            cv.Optional(CONF_BUS_CAPTURE, default=False): cv.boolean,
            # Plan 3.7.1: "If no valid frame has been seen for timeout (default
            # 60s), publish NAN ... and mark the component failed", gated on
            # MAIN specifically (ticket 07), not on HMI traffic.
            cv.Optional(CONF_TIMEOUT, default="60s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_HMI): SIDE_SCHEMA,
            cv.Optional(CONF_MAIN): SIDE_SCHEMA,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_sides,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_mode(config[CONF_MODE]))
    cg.add(var.set_bus_capture(config[CONF_BUS_CAPTURE]))
    cg.add(var.set_timeout(config[CONF_TIMEOUT]))

    side = config.get(CONF_HMI, config.get(CONF_MAIN))
    cg.add(var.set_uart_num(side[CONF_UART_NUM]))
    cg.add(var.set_rx_pin(side[CONF_RX_PIN]))
    if CONF_TX_PIN in side:
        cg.add(var.set_tx_pin(side[CONF_TX_PIN]))
