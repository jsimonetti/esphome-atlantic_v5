import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_ID, CONF_MODE, CONF_RX_PIN, CONF_TX_PIN

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

CONF_HMI = "hmi"
CONF_MAIN = "main"
CONF_UART_NUM = "uart_num"
CONF_BUS_CAPTURE = "bus_capture"

# M0.5: only the pins actually consumed by listener-mode bus_capture (3.5.5).
# tx_enable_pin / one_wire_mirror are not accepted yet: they land with M6, once
# mitm transport exists to consume them.
SIDE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_UART_NUM): cv.int_,
        cv.Required(CONF_RX_PIN): pins.internal_gpio_input_pin_number,
        cv.Optional(CONF_TX_PIN): pins.internal_gpio_output_pin_number,
    }
)


def _validate_sides(config):
    if config[CONF_MODE] == MODES[MODE_MITM]:
        # MITM transport (3.5, 3.6) isn't built yet; don't accept a config that
        # would silently do nothing.
        raise cv.Invalid("mode: mitm is not implemented yet (lands at M6); use mode: listener")
    has_hmi = CONF_HMI in config
    has_main = CONF_MAIN in config
    if has_hmi == has_main:  # neither, or both
        raise cv.Invalid("mode: listener requires exactly one of 'hmi:' or 'main:'")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AtlanticV5Component),
            cv.Optional(CONF_MODE, default=MODE_LISTENER): cv.enum(MODES, lower=True),
            cv.Optional(CONF_BUS_CAPTURE, default=False): cv.boolean,
            cv.Optional(CONF_HMI): SIDE_SCHEMA,
            cv.Optional(CONF_MAIN): SIDE_SCHEMA,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_sides,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_mode(config[CONF_MODE]))
    cg.add(var.set_bus_capture(config[CONF_BUS_CAPTURE]))

    side = config.get(CONF_HMI, config.get(CONF_MAIN))
    cg.add(var.set_uart_num(side[CONF_UART_NUM]))
    cg.add(var.set_rx_pin(side[CONF_RX_PIN]))
    if CONF_TX_PIN in side:
        cg.add(var.set_tx_pin(side[CONF_TX_PIN]))
