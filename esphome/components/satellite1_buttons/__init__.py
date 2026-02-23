import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation, pins
from esphome.const import CONF_ID, CONF_NAME, CONF_ICON, CONF_PIN, CONF_TRIGGER_ID

from esphome.components.satellite1.satellite1 import (
    namespace as sat_ns,
    Satellite1,
    Satellite1SPIService,
    CONF_SATELLITE1,
)

CODEOWNERS = ["@gnumpi"]

DEPENDENCIES = ["satellite1"]

# C++ class references
Satellite1ButtonManager = sat_ns.class_(
    "Satellite1ButtonManager", Satellite1SPIService, cg.Component
)
Satellite1Button = sat_ns.class_("Satellite1Button")
ComboConfig = sat_ns.class_("ComboConfig")

# Config keys
CONF_BUTTONS = "buttons"
CONF_COMBOS = "combos"
CONF_PORT = "port"
CONF_GPIO = "gpio"
CONF_DEBOUNCE = "debounce"
CONF_MULTI_CLICK_WINDOW = "multi_click_window"
CONF_REPEAT_INTERVAL = "repeat_interval"
CONF_HOLD_DURATION = "hold_duration"

# Trigger config keys
CONF_ON_PRESS = "on_press"
CONF_ON_RELEASE = "on_release"
CONF_ON_SINGLE_CLICK = "on_single_click"
CONF_ON_DOUBLE_CLICK = "on_double_click"
CONF_ON_TRIPLE_CLICK = "on_triple_click"
CONF_ON_HOLD_REPEAT = "on_hold_repeat"
CONF_ON_HOLD = "on_hold"
CONF_ON_COMBO = "on_combo"
CONF_DURATION = "duration"

# XMOS port enum values
XMOS_PORTS = {
    "INPUT_A": 0,
    "INPUT_B": 1,
    "OUTPUT_A": 2,
}

# Hold threshold sub-schema
HOLD_THRESHOLD_SCHEMA = automation.validate_automation(
    {
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
            automation.Trigger.template()
        ),
        cv.Required(CONF_DURATION): cv.positive_time_period_milliseconds,
    }
)

# Per-button schema
BUTTON_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Satellite1Button),
        cv.Required(CONF_NAME): cv.string,
        cv.Optional(CONF_ICON): cv.icon,
        # XMOS SPI button: port + pin
        cv.Optional(CONF_PORT): cv.one_of(*XMOS_PORTS, upper=True, space="_"),
        cv.Optional(CONF_PIN): cv.int_range(min=0, max=7),
        # Native ESP32 GPIO button
        cv.Optional(CONF_GPIO): pins.internal_gpio_input_pin_schema,
        # Per-button overrides
        cv.Optional(CONF_DEBOUNCE): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_MULTI_CLICK_WINDOW): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_REPEAT_INTERVAL): cv.positive_time_period_milliseconds,
        # Trigger automations
        cv.Optional(CONF_ON_PRESS): automation.validate_automation(),
        cv.Optional(CONF_ON_RELEASE): automation.validate_automation(),
        cv.Optional(CONF_ON_SINGLE_CLICK): automation.validate_automation(),
        cv.Optional(CONF_ON_DOUBLE_CLICK): automation.validate_automation(),
        cv.Optional(CONF_ON_TRIPLE_CLICK): automation.validate_automation(),
        cv.Optional(CONF_ON_HOLD_REPEAT): automation.validate_automation(),
        cv.Optional(CONF_ON_HOLD): HOLD_THRESHOLD_SCHEMA,
    }
)


def validate_button(value):
    """Validate that a button has either port+pin or gpio, but not both."""
    has_port = CONF_PORT in value
    has_gpio = CONF_GPIO in value
    if has_port == has_gpio:
        raise cv.Invalid(
            "Each button must have either 'port' + 'pin' (for XMOS SPI) "
            "or 'gpio' (for native ESP32), but not both."
        )
    if has_port and CONF_PIN not in value:
        raise cv.Invalid("XMOS SPI buttons require both 'port' and 'pin'.")
    return value


# Combo schema
COMBO_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ComboConfig),
        cv.Required(CONF_BUTTONS): cv.ensure_list(cv.use_id(Satellite1Button)),
        cv.Required(CONF_HOLD_DURATION): cv.positive_time_period_milliseconds,
        cv.Required(CONF_ON_COMBO): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                    automation.Trigger.template()
                ),
            }
        ),
    }
)

# Top-level component schema
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Satellite1ButtonManager),
        cv.GenerateID(CONF_SATELLITE1): cv.use_id(Satellite1),
        # Global defaults
        cv.Optional(CONF_DEBOUNCE, default="20ms"): cv.positive_time_period_milliseconds,
        cv.Optional(
            CONF_MULTI_CLICK_WINDOW, default="300ms"
        ): cv.positive_time_period_milliseconds,
        # Button list
        cv.Required(CONF_BUTTONS): cv.All(
            cv.ensure_list(BUTTON_SCHEMA), [validate_button]
        ),
        # Combo list (optional)
        cv.Optional(CONF_COMBOS): cv.ensure_list(COMBO_SCHEMA),
    }
)


async def _build_trigger_automations(btn_var, config, trigger_key, getter_name):
    """Helper to wire automations from config to a trigger getter on the button."""
    for conf in config.get(trigger_key, []):
        trigger = cg.MockObj(f"{btn_var}->{getter_name}()", "")
        await automation.build_automation(trigger, [], conf)


async def to_code(config):
    # Create manager
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_SATELLITE1])

    global_debounce = config[CONF_DEBOUNCE]
    global_mcw = config[CONF_MULTI_CLICK_WINDOW]

    # Create buttons
    for btn_conf in config[CONF_BUTTONS]:
        btn_var = cg.new_Pvariable(btn_conf[CONF_ID])
        cg.add(btn_var.set_name(btn_conf[CONF_NAME]))

        if CONF_ICON in btn_conf:
            cg.add(btn_var.set_icon(btn_conf[CONF_ICON]))

        if CONF_PORT in btn_conf:
            port_enum = cg.RawExpression(
                f"satellite1::XMOSPort::{btn_conf[CONF_PORT]}"
            )
            cg.add(btn_var.set_port(port_enum))
            cg.add(btn_var.set_pin(btn_conf[CONF_PIN]))
        elif CONF_GPIO in btn_conf:
            gpio_pin = await cg.gpio_pin_expression(btn_conf[CONF_GPIO])
            cg.add(btn_var.set_native_gpio(gpio_pin))

        # Debounce: per-button override or global default
        debounce = btn_conf.get(CONF_DEBOUNCE, global_debounce)
        cg.add(btn_var.set_debounce_ms(debounce))

        # Multi-click window: per-button override or global default
        mcw = btn_conf.get(CONF_MULTI_CLICK_WINDOW, global_mcw)
        cg.add(btn_var.set_multi_click_window_ms(mcw))

        # Repeat interval (optional, defaults to 0 = disabled)
        if CONF_REPEAT_INTERVAL in btn_conf:
            cg.add(btn_var.set_repeat_interval_ms(btn_conf[CONF_REPEAT_INTERVAL]))

        # Wire trigger automations
        await _build_trigger_automations(
            btn_var, btn_conf, CONF_ON_PRESS, "get_press_trigger"
        )
        await _build_trigger_automations(
            btn_var, btn_conf, CONF_ON_RELEASE, "get_release_trigger"
        )
        await _build_trigger_automations(
            btn_var, btn_conf, CONF_ON_SINGLE_CLICK, "get_single_click_trigger"
        )
        await _build_trigger_automations(
            btn_var, btn_conf, CONF_ON_DOUBLE_CLICK, "get_double_click_trigger"
        )
        await _build_trigger_automations(
            btn_var, btn_conf, CONF_ON_TRIPLE_CLICK, "get_triple_click_trigger"
        )
        await _build_trigger_automations(
            btn_var, btn_conf, CONF_ON_HOLD_REPEAT, "get_hold_repeat_trigger"
        )

        # Hold thresholds (each is an automation with a duration)
        for hold_conf in btn_conf.get(CONF_ON_HOLD, []):
            duration = hold_conf[CONF_DURATION]
            # Create a Trigger<> on the heap for this hold threshold
            hold_trigger = cg.new_Pvariable(hold_conf[CONF_TRIGGER_ID])
            cg.add(btn_var.add_hold_threshold(duration, hold_trigger))
            await automation.build_automation(hold_trigger, [], hold_conf)

        # Register button with manager
        cg.add(var.add_button(btn_var))

    # Create combos
    for combo_conf in config.get(CONF_COMBOS, []):
        combo_var = cg.new_Pvariable(combo_conf[CONF_ID])
        cg.add(combo_var.set_hold_duration_ms(combo_conf[CONF_HOLD_DURATION]))

        # Resolve button IDs
        for btn_id in combo_conf[CONF_BUTTONS]:
            btn_ref = await cg.get_variable(btn_id)
            cg.add(combo_var.add_button(btn_ref))

        # Create combo trigger and wire automations
        for on_combo_conf in combo_conf[CONF_ON_COMBO]:
            combo_trigger = cg.new_Pvariable(on_combo_conf[CONF_TRIGGER_ID])
            cg.add(combo_var.set_trigger(combo_trigger))
            await automation.build_automation(combo_trigger, [], on_combo_conf)

        cg.add(var.add_combo(combo_var))
