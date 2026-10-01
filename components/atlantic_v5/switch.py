import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_ATLANTIC_V5_ID, AtlanticV5Component, atlantic_v5_ns

DEPENDENCIES = ["atlantic_v5"]
CODEOWNERS = ["@jsimonetti"]

# Controllable entity (ADR 0001): write_state() writes into the hub's
# log_raw_frames_ flag, so it gets its own dedicated class instead of the
# read-only platforms' generic set_entity() table. The log_raw_frames
# switch gates whether already-framed, CRC-valid frames get hex-logged at
# DEBUG level.
AtlanticV5LogRawFramesSwitch = atlantic_v5_ns.class_("AtlanticV5LogRawFramesSwitch", switch.Switch, cg.Component)

CONF_LOG_RAW_FRAMES = "log_raw_frames"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ATLANTIC_V5_ID): cv.use_id(AtlanticV5Component),
        cv.Optional(CONF_LOG_RAW_FRAMES): switch.switch_schema(
            AtlanticV5LogRawFramesSwitch,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_ATLANTIC_V5_ID])
    if CONF_LOG_RAW_FRAMES not in config:
        return
    conf = config[CONF_LOG_RAW_FRAMES]
    var = await switch.new_switch(conf)
    await cg.register_component(var, conf)
    cg.add(var.set_parent(hub))
