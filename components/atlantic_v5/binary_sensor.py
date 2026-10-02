import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import DEVICE_CLASS_CONNECTIVITY, ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_ATLANTIC_V5_ID, AtlanticV5Component, EntityKind

DEPENDENCIES = ["atlantic_v5"]
CODEOWNERS = ["@jsimonetti"]


def _diagnostic_schema():
    return binary_sensor.binary_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC)


# key (message catalogue's Entity name, docs/protocol.md) -> (EntityId enum member
# name in catalog.h, schema). heating_active is a plain operational entity;
# the rest are diagnostic.
BINARY_SENSORS = {
    "heating_active": ("ENT_HEATING_ACTIVE", binary_sensor.binary_sensor_schema()),
    # Assumed mapping, not established - see docs/protocol.md.
    "heating_element_active": ("ENT_HEATING_ELEMENT_ACTIVE", binary_sensor.binary_sensor_schema()),
    "input_i1": ("ENT_INPUT_I1", _diagnostic_schema()),
    "input_i2": ("ENT_INPUT_I2", _diagnostic_schema()),
    "cycle_1_active": ("ENT_CYCLE_1_ACTIVE", _diagnostic_schema()),
    "cycle_2_active": ("ENT_CYCLE_2_ACTIVE", _diagnostic_schema()),
    "cycle_3_active": ("ENT_CYCLE_3_ACTIVE", _diagnostic_schema()),
    "cycle_4_active": ("ENT_CYCLE_4_ACTIVE", _diagnostic_schema()),
    "cycle_5_active": ("ENT_CYCLE_5_ACTIVE", _diagnostic_schema()),
    "cycle_6_active": ("ENT_CYCLE_6_ACTIVE", _diagnostic_schema()),
    # Not decoded from a frame: the staleness gate's own state.
    "connected": (
        "ENT_CONNECTED",
        binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    ),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ATLANTIC_V5_ID): cv.use_id(AtlanticV5Component),
        **{cv.Optional(key): schema for key, (_ent_id, schema) in BINARY_SENSORS.items()},
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_ATLANTIC_V5_ID])
    for key, (ent_id, _schema) in BINARY_SENSORS.items():
        if key not in config:
            continue
        sens = await binary_sensor.new_binary_sensor(config[key])
        # See sensor.py's comment: leading `::` disambiguates the L1/L2 core's
        # global `atlantic_v5` namespace from this file's `esphome::atlantic_v5`.
        cg.add(hub.set_entity(cg.RawExpression(f"::atlantic_v5::{ent_id}"), sens, EntityKind.BINARY_SENSOR))
