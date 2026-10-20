import esphome.codegen as cg
from esphome import final_validate
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_TEMPERATURE,
    UNIT_CELSIUS,
)

from . import CONF_ATLANTIC_V5_ID, CONF_MODE, MODE_MITM, AtlanticV5Component, atlantic_v5_ns

DEPENDENCIES = ["atlantic_v5"]
CODEOWNERS = ["@jsimonetti"]

# Controllable entity (ADR 0001): control() writes into the hub's RelayPolicy.
AtlanticV5Number = atlantic_v5_ns.class_("AtlanticV5Number", number.Number, cg.Component)

CONF_WATER_TEMPERATURE_OVERRIDE = "water_temperature_override"

# 0 means "no override"; the rewrite hook only fires for a non-zero value.
MIN_VALUE = 0.0
MAX_VALUE = 100.0
STEP = 0.1

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ATLANTIC_V5_ID): cv.use_id(AtlanticV5Component),
        cv.Optional(CONF_WATER_TEMPERATURE_OVERRIDE): number.number_schema(
            AtlanticV5Number,
            unit_of_measurement=UNIT_CELSIUS,
            device_class=DEVICE_CLASS_TEMPERATURE,
        ),
    }
)


def _final_validate(config):
    if CONF_WATER_TEMPERATURE_OVERRIDE not in config:
        return
    fconf = final_validate.full_config.get()
    hub_path = fconf.get_path_for_id(config[CONF_ATLANTIC_V5_ID])[:-1]
    hub_conf = fconf.get_config_for_path(hub_path)
    if hub_conf[CONF_MODE] != MODE_MITM:
        raise cv.Invalid(
            "water_temperature_override requires atlantic_v5 mode: mitm (rewriting frames only works in MITM mode)",
            path=[CONF_WATER_TEMPERATURE_OVERRIDE],
        )


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    hub = await cg.get_variable(config[CONF_ATLANTIC_V5_ID])
    if CONF_WATER_TEMPERATURE_OVERRIDE not in config:
        return
    conf = config[CONF_WATER_TEMPERATURE_OVERRIDE]
    var = await number.new_number(
        conf, min_value=MIN_VALUE, max_value=MAX_VALUE, step=STEP
    )
    await cg.register_component(var, conf)
    cg.add(var.set_parent(hub))
