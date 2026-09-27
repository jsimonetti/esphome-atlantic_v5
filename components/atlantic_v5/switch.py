import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_ATLANTIC_V5_ID, AtlanticV5Component, atlantic_v5_ns

DEPENDENCIES = ["atlantic_v5"]
CODEOWNERS = ["@jsimonetti"]

# Controllable entity (ADR 0001): write_state() writes into the hub's
# raw_frame_dump_ flag, so it gets its own dedicated class instead of the
# read-only platforms' generic set_entity() table. The raw_frame_dump
# switch gates whether already-framed, already-decoded frames get hex-dumped
# to the last_frame_dump text sensor (see text_sensor.py).
AtlanticV5RawFrameDumpSwitch = atlantic_v5_ns.class_("AtlanticV5RawFrameDumpSwitch", switch.Switch, cg.Component)

CONF_RAW_FRAME_DUMP = "raw_frame_dump"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ATLANTIC_V5_ID): cv.use_id(AtlanticV5Component),
        cv.Optional(CONF_RAW_FRAME_DUMP): switch.switch_schema(
            AtlanticV5RawFrameDumpSwitch,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_ATLANTIC_V5_ID])
    if CONF_RAW_FRAME_DUMP not in config:
        return
    conf = config[CONF_RAW_FRAME_DUMP]
    var = await switch.new_switch(conf)
    await cg.register_component(var, conf)
    cg.add(var.set_parent(hub))
