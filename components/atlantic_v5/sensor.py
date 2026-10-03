import esphome.codegen as cg
from esphome import final_validate
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_DISABLED_BY_DEFAULT,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_BYTES,
    UNIT_CELSIUS,
)

from . import (
    CONF_ATLANTIC_V5_ID,
    CONF_MODE,
    MODE_MITM,
    AtlanticV5Component,
    EntityKind,
)

DEPENDENCIES = ["atlantic_v5"]
CODEOWNERS = ["@jsimonetti"]


def _temp_schema(diagnostic=False):
    kwargs = {
        "unit_of_measurement": UNIT_CELSIUS,
        "accuracy_decimals": 2,
        "device_class": DEVICE_CLASS_TEMPERATURE,
        "state_class": STATE_CLASS_MEASUREMENT,
    }
    if diagnostic:
        kwargs["entity_category"] = ENTITY_CATEGORY_DIAGNOSTIC
    return sensor.sensor_schema(**kwargs)


def _count_schema():
    return sensor.sensor_schema(accuracy_decimals=0, entity_category=ENTITY_CATEGORY_DIAGNOSTIC)


def _debug_schema(unit):
    # Instrumentation of this bridge, not of the appliance: opt-in. The unit is
    # what keeps it out of Home Assistant's logbook, which logs every change of a
    # sensor carrying no unit, no state_class and no numeric device_class - and
    # these tick at 1 Hz. Any non-empty string does; without a device_class HA
    # neither validates nor converts it.
    return sensor.sensor_schema(
        unit_of_measurement=unit,
        accuracy_decimals=0,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ).extend({cv.Optional(CONF_DISABLED_BY_DEFAULT, default=True, visibility=cv.Visibility.ADVANCED): cv.boolean})


# key (also the message catalogue's Entity name, docs/protocol.md) -> (EntityId enum
# member name in catalog.h, schema). Kept in this order so it's easy to
# eyeball against catalog.h's EntityId; see build-and-tooling notes on keeping
# the two in lockstep.
SENSORS = {
    "setpoint": ("ENT_SETPOINT", _temp_schema()),
    "water_temperature": ("ENT_WATER_TEMPERATURE", _temp_schema()),
    "compressor_outlet_temperature": ("ENT_COMPRESSOR_OUTLET_TEMPERATURE", _temp_schema()),
    "air_inlet_temperature": ("ENT_AIR_INLET_TEMPERATURE", _temp_schema()),
    "evaporator_1_temperature": ("ENT_EVAPORATOR_1_TEMPERATURE", _temp_schema()),
    "evaporator_2_temperature": ("ENT_EVAPORATOR_2_TEMPERATURE", _temp_schema()),
    "evaporator_3_temperature": ("ENT_EVAPORATOR_3_TEMPERATURE", _temp_schema()),
    "water_temperature_min": ("ENT_WATER_TEMPERATURE_MIN", _temp_schema(True)),
    "water_temperature_max": ("ENT_WATER_TEMPERATURE_MAX", _temp_schema(True)),
    "compressor_outlet_temperature_min": ("ENT_COMPRESSOR_OUTLET_TEMPERATURE_MIN", _temp_schema(True)),
    "compressor_outlet_temperature_max": ("ENT_COMPRESSOR_OUTLET_TEMPERATURE_MAX", _temp_schema(True)),
    "air_inlet_temperature_min": ("ENT_AIR_INLET_TEMPERATURE_MIN", _temp_schema(True)),
    "air_inlet_temperature_max": ("ENT_AIR_INLET_TEMPERATURE_MAX", _temp_schema(True)),
    "evaporator_1_temperature_min": ("ENT_EVAPORATOR_1_TEMPERATURE_MIN", _temp_schema(True)),
    "evaporator_1_temperature_max": ("ENT_EVAPORATOR_1_TEMPERATURE_MAX", _temp_schema(True)),
    "evaporator_2_temperature_min": ("ENT_EVAPORATOR_2_TEMPERATURE_MIN", _temp_schema(True)),
    "evaporator_2_temperature_max": ("ENT_EVAPORATOR_2_TEMPERATURE_MAX", _temp_schema(True)),
    "evaporator_3_temperature_min": ("ENT_EVAPORATOR_3_TEMPERATURE_MIN", _temp_schema(True)),
    "evaporator_3_temperature_max": ("ENT_EVAPORATOR_3_TEMPERATURE_MAX", _temp_schema(True)),
    "cycle_1_count": ("ENT_CYCLE_1_COUNT", _count_schema()),
    "cycle_2_count": ("ENT_CYCLE_2_COUNT", _count_schema()),
    "cycle_3_count": ("ENT_CYCLE_3_COUNT", _count_schema()),
    "cycle_4_count": ("ENT_CYCLE_4_COUNT", _count_schema()),
    "cycle_5_count": ("ENT_CYCLE_5_COUNT", _count_schema()),
    "cycle_6_count": ("ENT_CYCLE_6_COUNT", _count_schema()),
    # Framing diagnostics, off by default. One assembler per wire, so the set
    # that means anything depends on the mode: listener taps a single wire and
    # uses the unsuffixed keys, mitm sits between two and publishes each side
    # separately. _final_validate rejects the wrong set for the mode.
    "valid_frames": ("ENT_VALID_FRAMES", _debug_schema("frames")),
    "crc_errors": ("ENT_CRC_ERRORS", _debug_schema("errors")),
    "dropped_bytes": ("ENT_DROPPED_BYTES", _debug_schema(UNIT_BYTES)),
    "valid_frames_hmi": ("ENT_VALID_FRAMES_HMI", _debug_schema("frames")),
    "crc_errors_hmi": ("ENT_CRC_ERRORS_HMI", _debug_schema("errors")),
    "dropped_bytes_hmi": ("ENT_DROPPED_BYTES_HMI", _debug_schema(UNIT_BYTES)),
    "valid_frames_main": ("ENT_VALID_FRAMES_MAIN", _debug_schema("frames")),
    "crc_errors_main": ("ENT_CRC_ERRORS_MAIN", _debug_schema("errors")),
    "dropped_bytes_main": ("ENT_DROPPED_BYTES_MAIN", _debug_schema(UNIT_BYTES)),
    "unknown_frames": ("ENT_UNKNOWN_FRAMES", _debug_schema("frames")),
    # mitm-only; stay at 0 in listener mode (no Relay/RelayTask there).
    "frames_relayed": ("ENT_FRAMES_RELAYED", _debug_schema("frames")),
    "rewrites_applied": ("ENT_REWRITES_APPLIED", _debug_schema("rewrites")),
    "queue_overflows": ("ENT_QUEUE_OVERFLOWS", _debug_schema("overflows")),
    "relay_latency_max_us": ("ENT_RELAY_LATENCY_MAX_US", _debug_schema("\u03bcs")),
    "relay_latency_avg_us": ("ENT_RELAY_LATENCY_AVG_US", _debug_schema("\u03bcs")),
    "task_stack_free": ("ENT_TASK_STACK_FREE", _debug_schema(UNIT_BYTES)),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ATLANTIC_V5_ID): cv.use_id(AtlanticV5Component),
        **{cv.Optional(key): schema for key, (_ent_id, schema) in SENSORS.items()},
    }
)

# The three framing counters, which exist once per assembler. Listed here as
# well as in SENSORS because SENSORS has to stay literal for
# test/host/test_catalog_parity.py to parse it; the assert below is what keeps
# the two from drifting, since a key that falls out of the gate silently reads
# zero forever - the exact symptom these counters exist to make unambiguous.
_FRAMING_COUNTERS = ("valid_frames", "crc_errors", "dropped_bytes")
_REJECTED_IN_MITM = {base: f"{base}_hmi / {base}_main" for base in _FRAMING_COUNTERS}
_REJECTED_IN_LISTENER = {f"{base}_{side}": base for base in _FRAMING_COUNTERS for side in ("hmi", "main")}
assert not (_REJECTED_IN_MITM.keys() | _REJECTED_IN_LISTENER.keys()) - SENSORS.keys()


def _final_validate(config):
    fconf = final_validate.full_config.get()
    hub_path = fconf.get_path_for_id(config[CONF_ATLANTIC_V5_ID])[:-1]
    mode = fconf.get_config_for_path(hub_path)[CONF_MODE]
    rejected = _REJECTED_IN_MITM if mode == MODE_MITM else _REJECTED_IN_LISTENER
    for key, replacement in rejected.items():
        if key in config:
            raise cv.Invalid(
                f"{key} is not available in atlantic_v5 mode: {mode}; use {replacement}",
                path=[key],
            )


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    hub = await cg.get_variable(config[CONF_ATLANTIC_V5_ID])
    for key, (ent_id, _schema) in SENSORS.items():
        if key not in config:
            continue
        sens = await sensor.new_sensor(config[key])
        # Fully-qualified with a leading `::`: this component's L1/L2 core
        # lives in a plain, global `atlantic_v5` namespace, distinct from
        # this file's own `esphome::atlantic_v5` - see CONTEXT.md / ADR 0001's
        # neighbourhood. ESPHome's generated main.cpp does `using namespace
        # esphome;` at global scope, so an unqualified `atlantic_v5::` here
        # would be ambiguous between the two.
        cg.add(hub.set_entity(cg.RawExpression(f"::atlantic_v5::{ent_id}"), sens, EntityKind.SENSOR))
