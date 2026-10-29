import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import DEVICE_CLASS_TEMPERATURE, UNIT_CELSIUS

from . import CONF_ATLANTIC_V5_ID, AtlanticV5Component, atlantic_v5_ns

DEPENDENCIES = ["atlantic_v5"]
CODEOWNERS = ["@jsimonetti"]

# User-supplied parameter (CONTEXT.md): control() only feeds the hub's own V40
# calculation, never the relay, so it is valid in both modes.
AtlanticV5Number = atlantic_v5_ns.class_("AtlanticV5Number", number.Number, cg.Component)

CONF_COLD_WATER_TEMPERATURE = "cold_water_temperature"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ATLANTIC_V5_ID): cv.use_id(AtlanticV5Component),
        cv.Optional(CONF_COLD_WATER_TEMPERATURE): number.number_schema(
            AtlanticV5Number,
            unit_of_measurement=UNIT_CELSIUS,
            device_class=DEVICE_CLASS_TEMPERATURE,
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_ATLANTIC_V5_ID])
    if CONF_COLD_WATER_TEMPERATURE not in config:
        return
    conf = config[CONF_COLD_WATER_TEMPERATURE]
    var = await number.new_number(conf, min_value=0.0, max_value=30.0, step=0.1)
    await cg.register_component(var, conf)
    cg.add(var.set_parent(hub))
