import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
)

from . import CONF_ATLANTIC_V5_ID, AtlanticV5Component, EntityKind

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


def _us_schema():
    return sensor.sensor_schema(
        unit_of_measurement="us",
        accuracy_decimals=0,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    )


# key (also the message catalogue's Entity name, plan 2.7) -> (EntityId enum
# member name in core/catalog.h, schema). Kept in this order so it's easy to
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
    # Diagnostics (plan 3.8), off by default: frames_ok/crc_errors/resyncs/
    # dropped_bytes are per-side totals in mitm mode, single-assembler totals
    # in listener mode (see AtlanticV5Component::update_diagnostics).
    "frames_ok": ("ENT_FRAMES_OK", _count_schema()),
    "crc_errors": ("ENT_CRC_ERRORS", _count_schema()),
    "resyncs": ("ENT_RESYNCS", _count_schema()),
    "dropped_bytes": ("ENT_DROPPED_BYTES", _count_schema()),
    "unknown_frames": ("ENT_UNKNOWN_FRAMES", _count_schema()),
    # mitm-only; stay at 0 in listener mode (no Relay/RelayTask there).
    "frames_relayed": ("ENT_FRAMES_RELAYED", _count_schema()),
    "rewrites_applied": ("ENT_REWRITES_APPLIED", _count_schema()),
    "echo_bytes": ("ENT_ECHO_BYTES", _count_schema()),
    "queue_overflows": ("ENT_QUEUE_OVERFLOWS", _count_schema()),
    "relay_latency_max_us": ("ENT_RELAY_LATENCY_MAX_US", _us_schema()),
    "relay_latency_avg_us": ("ENT_RELAY_LATENCY_AVG_US", _us_schema()),
    "task_stack_free": ("ENT_TASK_STACK_FREE", _count_schema()),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ATLANTIC_V5_ID): cv.use_id(AtlanticV5Component),
        **{cv.Optional(key): schema for key, (_ent_id, schema) in SENSORS.items()},
    }
)


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
