import esphome.config_validation as cv
import esphome.codegen as cg
from esphome import automation
from esphome.components import esp32
from esphome.const import CONF_ID, CONF_PORT

DEPENDENCIES = ["network", "audio", "mdns"]
CODEOWNERS = ["@FutureProofHomes"]

sendspin_ns = cg.esphome_ns.namespace("sendspin")
SendspinClient = sendspin_ns.class_("SendspinClient", cg.Component)

EnableAction = sendspin_ns.class_(
    "EnableAction", automation.Action, cg.Parented.template(SendspinClient)
)
DisableAction = sendspin_ns.class_(
    "DisableAction", automation.Action, cg.Parented.template(SendspinClient)
)
PlayPauseAction = sendspin_ns.class_(
    "PlayPauseAction", automation.Action, cg.Parented.template(SendspinClient)
)
NextAction = sendspin_ns.class_(
    "NextAction", automation.Action, cg.Parented.template(SendspinClient)
)
PreviousAction = sendspin_ns.class_(
    "PreviousAction", automation.Action, cg.Parented.template(SendspinClient)
)
SetVolumeAction = sendspin_ns.class_(
    "SetVolumeAction", automation.Action, cg.Parented.template(SendspinClient)
)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(SendspinClient),
    cv.Optional(CONF_PORT, default=8928): cv.port,
})

SENDSPIN_ACTION_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.use_id(SendspinClient),
})

SENDSPIN_SET_VOLUME_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.use_id(SendspinClient),
    cv.Required("volume"): cv.templatable(cv.percentage),
})


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID], config[CONF_PORT])
    await cg.register_component(var, config)
    cg.add_define("USE_SENDSPIN", True)
    esp32.add_idf_sdkconfig_option("CONFIG_HTTPD_WS_SUPPORT", True)


@automation.register_action("sendspin.enable", EnableAction, SENDSPIN_ACTION_SCHEMA)
@automation.register_action("sendspin.disable", DisableAction, SENDSPIN_ACTION_SCHEMA)
@automation.register_action("sendspin.play_pause", PlayPauseAction, SENDSPIN_ACTION_SCHEMA)
@automation.register_action("sendspin.next", NextAction, SENDSPIN_ACTION_SCHEMA)
@automation.register_action("sendspin.previous", PreviousAction, SENDSPIN_ACTION_SCHEMA)
async def sendspin_action(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action("sendspin.set_volume", SetVolumeAction, SENDSPIN_SET_VOLUME_SCHEMA)
async def sendspin_set_volume_action(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    template_ = await cg.templatable(config["volume"], args, float)
    cg.add(var.set_volume(template_))
    return var
