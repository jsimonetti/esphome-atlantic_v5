import esphome.codegen as cg
from esphome import final_validate
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import (
    CONF_DIRECTION,
    CONF_DURATION,
    CONF_RESTORE_MODE,
    ENTITY_CATEGORY_CONFIG,
)

from . import CONF_ATLANTIC_V5_ID, CONF_MODE, MODE_MITM, AtlanticV5Component, atlantic_v5_ns

DEPENDENCIES = ["atlantic_v5"]
CODEOWNERS = ["@jsimonetti"]

# Controllable entity (ADR 0001): write_state() reaches shared runtime state the
# relay task reads, so it gets its own dedicated class.
AtlanticV5LinkBlackoutSwitch = atlantic_v5_ns.class_(
    "AtlanticV5LinkBlackoutSwitch", switch.Switch, cg.Component
)

CONF_LINK_BLACKOUT = "link_blackout"

DIRECTION_MAIN_TO_HMI = "main_to_hmi"
DIRECTION_HMI_TO_MAIN = "hmi_to_main"
DIRECTION_BOTH = "both"
DIRECTIONS = {
    DIRECTION_MAIN_TO_HMI: "MAIN_TO_HMI",
    DIRECTION_HMI_TO_MAIN: "HMI_TO_MAIN",
    DIRECTION_BOTH: "BOTH",
}

# Duplicated from relay.h's DEFAULT_BLACKOUT_US so the value shows up in the
# resolved YAML.
DEFAULT_DURATION = "15s"
# The relay auto-releases on its own clock, so this bound is not what keeps the
# appliance safe - it is there so a typo can't ask for a blackout measured in
# hours.
MAX_DURATION_MS = 120_000


def _duration(value):
    value = cv.All(cv.positive_not_null_time_period, cv.positive_time_period_microseconds)(value)
    if value.total_milliseconds > MAX_DURATION_MS:
        raise cv.Invalid(f"duration must be at most {MAX_DURATION_MS // 1000}s")
    return value


def _no_restore(conf):
    # setup() always publishes off and never re-engages: a blackout that
    # survives a reboot is exactly the stranded-control-link failure the
    # auto-release exists to prevent.
    if conf[CONF_RESTORE_MODE] != "ALWAYS_OFF":
        raise cv.Invalid(
            "link_blackout is always off at boot and does not support restore_mode",
            path=[CONF_RESTORE_MODE],
        )
    return conf


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ATLANTIC_V5_ID): cv.use_id(AtlanticV5Component),
        cv.Optional(CONF_LINK_BLACKOUT): cv.All(
            switch.switch_schema(
                AtlanticV5LinkBlackoutSwitch,
                entity_category=ENTITY_CATEGORY_CONFIG,
            ).extend(
                {
                    cv.Optional(CONF_DIRECTION, default=DIRECTION_MAIN_TO_HMI): cv.one_of(
                        *DIRECTIONS, lower=True
                    ),
                    cv.Optional(CONF_DURATION, default=DEFAULT_DURATION): _duration,
                }
            ),
            _no_restore,
        ),
    }
)


def _final_validate(config):
    if CONF_LINK_BLACKOUT not in config:
        return
    fconf = final_validate.full_config.get()
    hub_path = fconf.get_path_for_id(config[CONF_ATLANTIC_V5_ID])[:-1]
    hub_conf = fconf.get_config_for_path(hub_path)
    if hub_conf[CONF_MODE] != MODE_MITM:
        raise cv.Invalid(
            "link_blackout requires atlantic_v5 mode: mitm (there is nothing to stop "
            "forwarding in a passive tap)",
            path=[CONF_LINK_BLACKOUT],
        )


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    hub = await cg.get_variable(config[CONF_ATLANTIC_V5_ID])
    if CONF_LINK_BLACKOUT not in config:
        return
    conf = config[CONF_LINK_BLACKOUT]
    # The gate is the feature: without this flag Relay has no drop path compiled
    # in, so a build whose config omits the entity cannot sever the control link.
    cg.add_build_flag("-DATLANTIC_V5_LINK_BLACKOUT")
    # Leading "::" for the same reason as the read-only platforms' ENT_* ids:
    # ESPHome's generated main.cpp does `using namespace esphome;`, which makes
    # an unqualified atlantic_v5:: ambiguous with esphome::atlantic_v5_component.
    direction = cg.RawExpression(f"::atlantic_v5::BlackoutDirection::{DIRECTIONS[conf[CONF_DIRECTION]]}")
    cg.add(hub.set_link_blackout_config(direction, conf[CONF_DURATION]))

    var = await switch.new_switch(conf)
    await cg.register_component(var, conf)
    cg.add(var.set_parent(hub))
