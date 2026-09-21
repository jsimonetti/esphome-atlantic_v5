import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MODE

CODEOWNERS = ["@jsimonetti"]

atlantic_v5_ns = cg.esphome_ns.namespace("atlantic_v5")
AtlanticV5Component = atlantic_v5_ns.class_("AtlanticV5Component", cg.Component)
Mode = atlantic_v5_ns.enum("Mode", is_class=True)

MODE_LISTENER = "listener"
MODE_MITM = "mitm"
MODES = {
    MODE_LISTENER: Mode.LISTENER,
    MODE_MITM: Mode.MITM,
}

# M0 skeleton: mode selection only. hmi:/main: pin schemas, entity platforms and
# per-mode validation rules (3.7.2) land with M4 (listener) and M6 (mitm).
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(AtlanticV5Component),
        cv.Optional(CONF_MODE, default=MODE_LISTENER): cv.enum(MODES, lower=True),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_mode(config[CONF_MODE]))
