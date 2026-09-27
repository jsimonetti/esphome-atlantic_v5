import esphome.codegen as cg
from esphome import final_validate
from esphome.components import select
import esphome.config_validation as cv

from . import CONF_ATLANTIC_V5_ID, CONF_MODE, MODE_MITM, AtlanticV5Component, atlantic_v5_ns

DEPENDENCIES = ["atlantic_v5"]
CODEOWNERS = ["@jsimonetti"]

# Controllable entity (ADR 0001): control() writes into the hub's RelayPolicy,
# so it gets its own dedicated class instead of the read-only platforms'
# generic set_entity() table.
AtlanticV5Select = atlantic_v5_ns.class_("AtlanticV5Select", select.Select, cg.Component)

CONF_CONTROL_MODE = "control_mode"

# The control surface's modes (docs/protocol.md), in ControlMode enum order
# (types.h): the option's
# index IS the wire value the rewrite hook applies, so no string lookup is
# needed on the hot control path (AtlanticV5Select::control()).
OPTION_PASSTHROUGH = "passthrough"
OPTION_NORMAL = "normal"
OPTION_EAGER = "eager"
OPTION_OFF = "off"
OPTION_BOOST = "boost"
OPTIONS = [OPTION_PASSTHROUGH, OPTION_NORMAL, OPTION_EAGER, OPTION_OFF, OPTION_BOOST]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ATLANTIC_V5_ID): cv.use_id(AtlanticV5Component),
        cv.Optional(CONF_CONTROL_MODE): select.select_schema(AtlanticV5Select),
    }
)


def _final_validate(config):
    if CONF_CONTROL_MODE not in config:
        return
    fconf = final_validate.full_config.get()
    hub_path = fconf.get_path_for_id(config[CONF_ATLANTIC_V5_ID])[:-1]
    hub_conf = fconf.get_config_for_path(hub_path)
    if hub_conf[CONF_MODE] != MODE_MITM:
        raise cv.Invalid(
            "control_mode requires atlantic_v5 mode: mitm (rewriting frames only works in MITM mode)",
            path=[CONF_CONTROL_MODE],
        )


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    hub = await cg.get_variable(config[CONF_ATLANTIC_V5_ID])
    if CONF_CONTROL_MODE not in config:
        return
    conf = config[CONF_CONTROL_MODE]
    var = await select.new_select(conf, options=OPTIONS)
    await cg.register_component(var, conf)
    cg.add(var.set_parent(hub))
