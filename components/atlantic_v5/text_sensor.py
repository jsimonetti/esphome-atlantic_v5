import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_ATLANTIC_V5_ID, AtlanticV5Component, EntityKind

DEPENDENCIES = ["atlantic_v5"]
CODEOWNERS = ["@jsimonetti"]

# key (message catalogue's Entity name, docs/protocol.md) -> EntityId enum member
# name in catalog.h. All diagnostic: version/model/serial info, not telemetry.
TEXT_SENSORS = {
    "firmware_version": "ENT_FIRMWARE_VERSION",
    "serial_number": "ENT_SERIAL_NUMBER",
    "power_board_version": "ENT_POWER_BOARD_VERSION",
    "controller_model": "ENT_CONTROLLER_MODEL",
    "hmi_version": "ENT_HMI_VERSION",
    "hmi_model": "ENT_HMI_MODEL",
    # Diagnostics, off by default.
    "last_unknown_frame": "ENT_LAST_UNKNOWN_FRAME",
    "last_frame_dump": "ENT_LAST_FRAME_DUMP",
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ATLANTIC_V5_ID): cv.use_id(AtlanticV5Component),
        **{
            cv.Optional(key): text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC)
            for key in TEXT_SENSORS
        },
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_ATLANTIC_V5_ID])
    for key, ent_id in TEXT_SENSORS.items():
        if key not in config:
            continue
        sens = await text_sensor.new_text_sensor(config[key])
        # See sensor.py's comment: leading `::` disambiguates the L1/L2 core's
        # global `atlantic_v5` namespace from this file's `esphome::atlantic_v5`.
        cg.add(hub.set_entity(cg.RawExpression(f"::atlantic_v5::{ent_id}"), sens, EntityKind.TEXT_SENSOR))
