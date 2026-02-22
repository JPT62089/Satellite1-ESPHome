# Satellite1 Buttons Component Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the YAML-based button handling with a custom C++ `satellite1_buttons` ESPHome component that provides clean gesture triggers (click, multi-click, hold, hold-repeat, combos).

**Architecture:** A `Satellite1ButtonManager` (Component + Satellite1SPIService) polls all button pins in one SPI read per loop, runs per-button state machines for debounce/gesture detection, and fires ESPHome `Trigger<>` callbacks. YAML config becomes purely declarative. Follows the existing `memory_flasher` sub-component pattern.

**Tech Stack:** C++ (ESPHome component API), Python (ESPHome codegen/config validation), YAML (ESPHome config)

**Design doc:** `docs/plans/2026-02-22-satellite1-buttons-design.md`

---

### Task 1: Scaffold the component — header with classes and enums

**Files:**
- Create: `esphome/components/satellite1/buttons/satellite1_buttons.h`

**Context:**
- The component follows the sub-component pattern from `esphome/components/satellite1/memory_flasher/`
- `Satellite1SPIService` (from `satellite1.h:166-177`) provides `this->parent_` for SPI access
- Button pins are read via `parent_->request_status_register_update()` + `parent_->get_dc_status(DC_STATUS_REGISTER::GPIO_PORT_IN_A)`
- The action button uses native ESP32 GPIO 0, not XMOS SPI

**Step 1: Write the header file**

```cpp
#pragma once

#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/hal.h"

#include "../satellite1.h"
#include "../sat_gpio.h"

namespace esphome {
namespace satellite1 {

static const char *const TAG_BUTTONS = "satellite1.buttons";

/// Per-button hold threshold: fires a trigger when held for at least `duration_ms`.
struct HoldThreshold {
  uint32_t duration_ms;
  Trigger<> *trigger;
  bool fired{false};  // reset on release
};

/// Per-button configuration and state machine for gesture detection.
class Satellite1Button {
 public:
  void set_pin(uint8_t pin) { this->pin_ = pin; }
  void set_port(XMOSPort port) { this->port_ = port; }
  void set_native_gpio(uint8_t gpio_num);
  void set_debounce_ms(uint32_t ms) { this->debounce_ms_ = ms; }
  void set_multi_click_window_ms(uint32_t ms) { this->multi_click_window_ms_ = ms; }
  void set_repeat_interval_ms(uint32_t ms) { this->repeat_interval_ms_ = ms; }
  void set_name(const std::string &name) { this->name_ = name; }

  void add_hold_threshold(HoldThreshold threshold) { this->hold_thresholds_.push_back(threshold); }

  // Trigger accessors for codegen
  Trigger<> *get_press_trigger() { return &this->press_trigger_; }
  Trigger<> *get_release_trigger() { return &this->release_trigger_; }
  Trigger<> *get_single_click_trigger() { return &this->single_click_trigger_; }
  Trigger<> *get_double_click_trigger() { return &this->double_click_trigger_; }
  Trigger<> *get_triple_click_trigger() { return &this->triple_click_trigger_; }
  Trigger<> *get_hold_repeat_trigger() { return &this->hold_repeat_trigger_; }

  /// Returns true if this button uses native ESP32 GPIO instead of XMOS SPI.
  bool is_native_gpio() const { return this->use_native_gpio_; }

  /// Read the raw button state from the given port value (for SPI buttons).
  bool read_from_port(uint8_t port_value) const { return !!(port_value & (1 << this->pin_)); }

  /// Read from native GPIO.
  bool read_native_gpio() const;

  /// Returns true if the button is currently in the pressed (debounced) state.
  bool is_pressed() const { return this->debounced_state_; }

  const std::string &get_name() const { return this->name_; }
  XMOSPort get_port() const { return this->port_; }

  /// Called every loop by the manager with the current raw pin state.
  void process(bool raw_pressed, uint32_t now_ms);

 protected:
  // Config
  std::string name_;
  uint8_t pin_{0};
  XMOSPort port_{XMOSPort::INPUT_A};
  bool use_native_gpio_{false};
  uint8_t native_gpio_num_{0};
  uint32_t debounce_ms_{20};
  uint32_t multi_click_window_ms_{250};
  uint32_t repeat_interval_ms_{0};  // 0 = no repeat
  std::vector<HoldThreshold> hold_thresholds_;

  // Debounce state
  bool raw_state_{false};
  bool debounced_state_{false};
  uint32_t last_raw_change_ms_{0};

  // Gesture state
  bool prev_debounced_{false};
  uint32_t press_start_ms_{0};
  uint8_t click_count_{0};
  uint32_t last_click_release_ms_{0};
  uint32_t last_repeat_ms_{0};
  bool hold_active_{false};  // true once any hold threshold fires

  // Triggers
  Trigger<> press_trigger_;
  Trigger<> release_trigger_;
  Trigger<> single_click_trigger_;
  Trigger<> double_click_trigger_;
  Trigger<> triple_click_trigger_;
  Trigger<> hold_repeat_trigger_;
};

/// Combo: fires when all specified buttons are held simultaneously.
struct ComboConfig {
  std::vector<Satellite1Button *> buttons;
  uint32_t hold_duration_ms{500};
  Trigger<> *trigger;
  bool fired{false};
  uint32_t all_pressed_since_ms_{0};
};

/// Manages all satellite1 buttons — single SPI poll per loop.
class Satellite1ButtonManager : public Component, public Satellite1SPIService {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void add_button(Satellite1Button *button) { this->buttons_.push_back(button); }
  void add_combo(ComboConfig combo) { this->combos_.push_back(std::move(combo)); }

  void set_enabled(bool enabled) { this->enabled_ = enabled; }
  bool is_enabled() const { return this->enabled_; }

  /// Returns true if any volume button was recently touched (for LED control).
  bool volume_buttons_touched() const { return this->volume_buttons_touched_; }
  void set_volume_buttons_touched(bool val) { this->volume_buttons_touched_ = val; }

 protected:
  void poll_buttons_();
  void check_combos_(uint32_t now_ms);

  std::vector<Satellite1Button *> buttons_;
  std::vector<ComboConfig> combos_;
  bool enabled_{false};
  bool volume_buttons_touched_{false};
};

}  // namespace satellite1
}  // namespace esphome
```

**Step 2: Verify the header compiles in isolation**

No compile check yet — we need the .cpp and __init__.py first. Continue to Task 2.

**Step 3: Commit**

```bash
git add esphome/components/satellite1/buttons/satellite1_buttons.h
git commit -m "feat(buttons): add satellite1_buttons header with manager and button classes"
```

---

### Task 2: Implement the C++ state machine

**Files:**
- Create: `esphome/components/satellite1/buttons/satellite1_buttons.cpp`

**Context:**
- `process()` runs the per-button debounce + gesture state machine described in the design doc
- SPI reads use `this->parent_->request_status_register_update()` and `this->parent_->get_dc_status(DC_STATUS_REGISTER::GPIO_PORT_IN_A)` — see `sat_gpio.cpp:17-37`
- Native GPIO reads use `digitalRead()` for the action button (ESP32 GPIO 0)
- All timing uses `millis()` (ESPHome's `esphome/core/hal.h`)

**Step 1: Write the implementation**

```cpp
#include "satellite1_buttons.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace satellite1 {

// --- Satellite1Button ---

void Satellite1Button::set_native_gpio(uint8_t gpio_num) {
  this->use_native_gpio_ = true;
  this->native_gpio_num_ = gpio_num;
  pinMode(gpio_num, INPUT_PULLUP);
}

bool Satellite1Button::read_native_gpio() const { return digitalRead(this->native_gpio_num_) == LOW; }

void Satellite1Button::process(bool raw_pressed, uint32_t now_ms) {
  // --- Debounce ---
  if (raw_pressed != this->raw_state_) {
    this->raw_state_ = raw_pressed;
    this->last_raw_change_ms_ = now_ms;
  }
  if ((now_ms - this->last_raw_change_ms_) < this->debounce_ms_) {
    // Still bouncing, use previous debounced state
    raw_pressed = this->debounced_state_;
  }
  bool prev = this->debounced_state_;
  this->debounced_state_ = raw_pressed;

  // --- Edge detection ---
  bool just_pressed = raw_pressed && !prev;
  bool just_released = !raw_pressed && prev;

  if (just_pressed) {
    this->press_start_ms_ = now_ms;
    this->hold_active_ = false;
    this->last_repeat_ms_ = now_ms;
    // Reset hold threshold fired flags
    for (auto &ht : this->hold_thresholds_) {
      ht.fired = false;
    }
    this->press_trigger_.trigger();
    ESP_LOGD(TAG_BUTTONS, "%s: press", this->name_.c_str());
  }

  if (just_released) {
    this->release_trigger_.trigger();
    ESP_LOGD(TAG_BUTTONS, "%s: release", this->name_.c_str());

    if (!this->hold_active_) {
      // No hold was triggered — count this as a click
      this->click_count_++;
      this->last_click_release_ms_ = now_ms;
    }
    this->hold_active_ = false;
  }

  // --- Hold detection (while pressed) ---
  if (raw_pressed && this->press_start_ms_ > 0) {
    uint32_t held_ms = now_ms - this->press_start_ms_;

    // Check hold thresholds
    for (auto &ht : this->hold_thresholds_) {
      if (!ht.fired && held_ms >= ht.duration_ms) {
        ht.fired = true;
        this->hold_active_ = true;
        ht.trigger->trigger();
        ESP_LOGD(TAG_BUTTONS, "%s: hold %ums", this->name_.c_str(), ht.duration_ms);
      }
    }

    // Hold-to-repeat
    if (this->repeat_interval_ms_ > 0 && held_ms >= this->debounce_ms_) {
      if ((now_ms - this->last_repeat_ms_) >= this->repeat_interval_ms_) {
        this->last_repeat_ms_ = now_ms;
        this->hold_repeat_trigger_.trigger();
        ESP_LOGD(TAG_BUTTONS, "%s: repeat", this->name_.c_str());
      }
    }
  }

  // --- Multi-click window ---
  if (this->click_count_ > 0 && !raw_pressed) {
    uint32_t since_release = now_ms - this->last_click_release_ms_;
    if (since_release >= this->multi_click_window_ms_) {
      switch (this->click_count_) {
        case 1:
          this->single_click_trigger_.trigger();
          ESP_LOGD(TAG_BUTTONS, "%s: single_click", this->name_.c_str());
          break;
        case 2:
          this->double_click_trigger_.trigger();
          ESP_LOGD(TAG_BUTTONS, "%s: double_click", this->name_.c_str());
          break;
        default:
          this->triple_click_trigger_.trigger();
          ESP_LOGD(TAG_BUTTONS, "%s: triple_click", this->name_.c_str());
          break;
      }
      this->click_count_ = 0;
    }
  }
}

// --- Satellite1ButtonManager ---

void Satellite1ButtonManager::setup() {
  ESP_LOGCONFIG(TAG_BUTTONS, "Setting up Satellite1 Button Manager...");
  ESP_LOGCONFIG(TAG_BUTTONS, "  Buttons: %d", this->buttons_.size());
  ESP_LOGCONFIG(TAG_BUTTONS, "  Combos: %d", this->combos_.size());
}

void Satellite1ButtonManager::dump_config() {
  ESP_LOGCONFIG(TAG_BUTTONS, "Satellite1 Button Manager:");
  ESP_LOGCONFIG(TAG_BUTTONS, "  Buttons: %d", this->buttons_.size());
  for (auto *btn : this->buttons_) {
    ESP_LOGCONFIG(TAG_BUTTONS, "    - %s (pin %d, %s)", btn->get_name().c_str(),
                  btn->is_native_gpio() ? 0 : 0,  // TODO: expose pin number getter
                  btn->is_native_gpio() ? "native GPIO" : "XMOS SPI");
  }
  ESP_LOGCONFIG(TAG_BUTTONS, "  Combos: %d", this->combos_.size());
}

void Satellite1ButtonManager::loop() {
  if (!this->enabled_)
    return;

  uint32_t now_ms = millis();
  this->poll_buttons_();

  for (auto *btn : this->buttons_) {
    bool raw;
    if (btn->is_native_gpio()) {
      raw = btn->read_native_gpio();
    } else {
      uint8_t port_value = this->parent_->get_dc_status(
          btn->get_port() == XMOSPort::INPUT_A ? DC_STATUS_REGISTER::GPIO_PORT_IN_A
                                               : DC_STATUS_REGISTER::GPIO_PORT_IN_B);
      raw = btn->read_from_port(port_value);
    }
    btn->process(raw, now_ms);
  }

  this->check_combos_(now_ms);
}

void Satellite1ButtonManager::poll_buttons_() {
  // Single SPI poll for all XMOS buttons
  this->parent_->request_status_register_update();
}

void Satellite1ButtonManager::check_combos_(uint32_t now_ms) {
  for (auto &combo : this->combos_) {
    bool all_pressed = true;
    for (auto *btn : combo.buttons) {
      if (!btn->is_pressed()) {
        all_pressed = false;
        break;
      }
    }

    if (all_pressed) {
      if (combo.all_pressed_since_ms_ == 0) {
        combo.all_pressed_since_ms_ = now_ms;
      }
      if (!combo.fired && (now_ms - combo.all_pressed_since_ms_) >= combo.hold_duration_ms) {
        combo.fired = true;
        combo.trigger->trigger();
        ESP_LOGD(TAG_BUTTONS, "Combo triggered");
      }
    } else {
      combo.all_pressed_since_ms_ = 0;
      combo.fired = false;
    }
  }
}

}  // namespace satellite1
}  // namespace esphome
```

**Step 2: Commit**

```bash
git add esphome/components/satellite1/buttons/satellite1_buttons.cpp
git commit -m "feat(buttons): implement button state machine and manager loop"
```

---

### Task 3: Write the Python config schema and codegen

**Files:**
- Create: `esphome/components/satellite1/buttons/__init__.py`

**Context:**
- Follow the `memory_flasher/__init__.py` pattern: import `from .. import satellite1 as sat`, declare class via `sat.namespace.class_()`, register parenting
- The schema needs to support: per-button config (pin, port, gpio, triggers), global timing defaults, combo config
- ESPHome automation triggers use `automation.validate_automation()` and `automation.build_automation()`
- See `satellite1.py:38-54` for how triggers are registered in the parent component's schema
- Timing values use `cv.positive_time_period_milliseconds` for YAML-friendly duration strings like `250ms`

**Step 1: Write the __init__.py**

```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID, CONF_NAME, CONF_ICON, CONF_PIN, CONF_PORT, CONF_TRIGGER_ID

from .. import satellite1 as sat

CODEOWNERS = ["@gnumpi"]
DEPENDENCIES = ["satellite1"]

# C++ class references
Satellite1ButtonManager = sat.namespace.class_(
    "Satellite1ButtonManager", sat.Satellite1SPIService, cg.Component
)
Satellite1Button = sat.namespace.class_("Satellite1Button")
ComboConfig = sat.namespace.struct("ComboConfig")

# Config keys
CONF_BUTTONS = "buttons"
CONF_COMBOS = "combos"
CONF_GPIO = "gpio"
CONF_DEBOUNCE = "debounce"
CONF_MULTI_CLICK_WINDOW = "multi_click_window"
CONF_REPEAT_INTERVAL = "repeat_interval"
CONF_HOLD_DURATION = "hold_duration"
CONF_THRESHOLD = "threshold"
CONF_ON_PRESS = "on_press"
CONF_ON_RELEASE = "on_release"
CONF_ON_SINGLE_CLICK = "on_single_click"
CONF_ON_DOUBLE_CLICK = "on_double_click"
CONF_ON_TRIPLE_CLICK = "on_triple_click"
CONF_ON_HOLD = "on_hold"
CONF_ON_HOLD_REPEAT = "on_hold_repeat"
CONF_ON_COMBO = "on_combo"

# Reuse XMOSPort enum from sat_gpio
from ..sat_gpio import XMOS_PORT

HOLD_THRESHOLD_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_THRESHOLD): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_TRIGGER_ID): cv.declare_id(automation.Trigger.template()),
        cv.Required("then"): automation.validate_automation(),
    }
)

HOLD_REPEAT_SCHEMA = cv.Schema(
    {
        cv.Optional(
            CONF_REPEAT_INTERVAL, default="150ms"
        ): cv.positive_time_period_milliseconds,
        cv.Required("then"): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                    automation.Trigger.template()
                ),
            }
        ),
    }
)

BUTTON_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Satellite1Button),
        cv.Optional(CONF_NAME, default=""): cv.string,
        cv.Optional(CONF_ICON, default=""): cv.icon,
        # Pin source — either XMOS SPI port+pin or native ESP32 GPIO
        cv.Optional(CONF_PORT): cv.enum(XMOS_PORT),
        cv.Optional(CONF_PIN): cv.int_range(min=0, max=7),
        cv.Optional(CONF_GPIO): cv.int_range(min=0, max=48),
        # Per-button timing overrides
        cv.Optional(CONF_DEBOUNCE): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_MULTI_CLICK_WINDOW): cv.positive_time_period_milliseconds,
        # Triggers
        cv.Optional(CONF_ON_PRESS): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(automation.Trigger.template())}
        ),
        cv.Optional(CONF_ON_RELEASE): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(automation.Trigger.template())}
        ),
        cv.Optional(CONF_ON_SINGLE_CLICK): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(automation.Trigger.template())}
        ),
        cv.Optional(CONF_ON_DOUBLE_CLICK): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(automation.Trigger.template())}
        ),
        cv.Optional(CONF_ON_TRIPLE_CLICK): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(automation.Trigger.template())}
        ),
        cv.Optional(CONF_ON_HOLD): cv.ensure_list(HOLD_THRESHOLD_SCHEMA),
        cv.Optional(CONF_ON_HOLD_REPEAT): HOLD_REPEAT_SCHEMA,
    }
)

COMBO_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_BUTTONS): cv.ensure_list(cv.use_id(Satellite1Button)),
        cv.Optional(
            CONF_HOLD_DURATION, default="500ms"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_ON_COMBO): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(automation.Trigger.template())}
        ),
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Satellite1ButtonManager),
        cv.GenerateID(sat.CONF_SATELLITE1): cv.use_id(sat.Satellite1),
        cv.Optional(CONF_DEBOUNCE, default="20ms"): cv.positive_time_period_milliseconds,
        cv.Optional(
            CONF_MULTI_CLICK_WINDOW, default="250ms"
        ): cv.positive_time_period_milliseconds,
        cv.Required(CONF_BUTTONS): cv.ensure_list(BUTTON_SCHEMA),
        cv.Optional(CONF_COMBOS, default=[]): cv.ensure_list(COMBO_SCHEMA),
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await cg.register_parented(var, config[sat.CONF_SATELLITE1])

    global_debounce = config[CONF_DEBOUNCE].total_milliseconds
    global_mcw = config[CONF_MULTI_CLICK_WINDOW].total_milliseconds

    for btn_conf in config[CONF_BUTTONS]:
        btn = cg.new_Pvariable(btn_conf[CONF_ID])

        # Pin config
        if CONF_GPIO in btn_conf:
            cg.add(btn.set_native_gpio(btn_conf[CONF_GPIO]))
        else:
            cg.add(btn.set_port(btn_conf[CONF_PORT]))
            cg.add(btn.set_pin(btn_conf[CONF_PIN]))

        if btn_conf[CONF_NAME]:
            cg.add(btn.set_name(btn_conf[CONF_NAME]))

        # Timing — per-button overrides or global defaults
        debounce = btn_conf.get(CONF_DEBOUNCE)
        if debounce is not None:
            cg.add(btn.set_debounce_ms(debounce.total_milliseconds))
        else:
            cg.add(btn.set_debounce_ms(global_debounce))

        mcw = btn_conf.get(CONF_MULTI_CLICK_WINDOW)
        if mcw is not None:
            cg.add(btn.set_multi_click_window_ms(mcw.total_milliseconds))
        else:
            cg.add(btn.set_multi_click_window_ms(global_mcw))

        # Simple triggers
        for trigger_key, getter_name in [
            (CONF_ON_PRESS, "get_press_trigger"),
            (CONF_ON_RELEASE, "get_release_trigger"),
            (CONF_ON_SINGLE_CLICK, "get_single_click_trigger"),
            (CONF_ON_DOUBLE_CLICK, "get_double_click_trigger"),
            (CONF_ON_TRIPLE_CLICK, "get_triple_click_trigger"),
        ]:
            for conf in btn_conf.get(trigger_key, []):
                trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
                cg.add(
                    trigger.set_parent(getattr(btn, getter_name)())
                )  # TODO: verify codegen pattern
                await automation.build_automation(trigger, [], conf)

        # Hold thresholds
        for hold_conf in btn_conf.get(CONF_ON_HOLD, []):
            trigger = cg.new_Pvariable(
                hold_conf.get(CONF_TRIGGER_ID, cg.new_id(automation.Trigger.template()))
            )
            threshold_ms = hold_conf[CONF_THRESHOLD].total_milliseconds
            # Build automation from 'then' key
            for auto_conf in hold_conf.get("then", []):
                await automation.build_automation(trigger, [], auto_conf)
            cg.add(
                btn.add_hold_threshold(
                    cg.StructInitializer(
                        ComboConfig,  # placeholder — will need HoldThreshold struct
                        ("duration_ms", threshold_ms),
                        ("trigger", trigger),
                    )
                )
            )

        # Hold repeat
        if CONF_ON_HOLD_REPEAT in btn_conf:
            hr_conf = btn_conf[CONF_ON_HOLD_REPEAT]
            cg.add(
                btn.set_repeat_interval_ms(
                    hr_conf[CONF_REPEAT_INTERVAL].total_milliseconds
                )
            )
            for conf in hr_conf.get("then", []):
                trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
                await automation.build_automation(trigger, [], conf)
                # Wire trigger to the button's hold_repeat trigger
                # TODO: verify this wiring pattern

        cg.add(var.add_button(btn))

    # Combos
    for combo_conf in config.get(CONF_COMBOS, []):
        # This will need refinement during implementation to match
        # ESPHome's codegen patterns for struct initialization
        pass

    return var
```

**Important notes for implementer:**
- The trigger wiring pattern (connecting YAML automation triggers to C++ `Trigger<>` objects) needs careful attention. The exact ESPHome codegen API for this should be verified by looking at how `binary_sensor` platform's `on_press` trigger works. Check `esphome/components/binary_sensor/__init__.py` for the canonical pattern.
- The `HoldThreshold` struct initialization via codegen may need a different approach than `StructInitializer` — verify with existing patterns.
- The `TODO` comments mark areas that need verification during implementation.

**Step 2: Verify the schema compiles**

Run: `source .venv/bin/activate && esphome config config/satellite1.yaml 2>&1 | head -50`

This will fail since buttons.yaml hasn't been updated yet, but ensures the Python imports work.

**Step 3: Commit**

```bash
git add esphome/components/satellite1/buttons/__init__.py
git commit -m "feat(buttons): add Python config schema and codegen for satellite1_buttons"
```

---

### Task 4: Write a minimal compile test

**Files:**
- Create: `tests/components/satellite1_buttons/test_buttons.yaml`

**Context:**
- Follow the pattern from `tests/components/fusb302b/test_power_delivery.yaml`
- The test should define the `satellite1_buttons` component with at least one XMOS SPI button and one native GPIO button
- It must include enough surrounding config (SPI bus, satellite1 component) to compile

**Step 1: Write the test YAML**

```yaml
substitutions:
  friendly_name: "Satellite1 Buttons Test"
  node_name: sat1-buttons-test
  company_name: FutureProofHomes
  project_name: Satellite1
  component_name: Core

esphome:
  name: ${node_name}
  name_add_mac_suffix: true
  friendly_name: ${friendly_name}
  min_version: 2026.1.0
  project:
    name: ${company_name}.${project_name}
    version: dev

packages:
  core_board: !include ../../config/common/core_board.yaml
  wifi: !include ../../config/common/wifi_improv.yaml

external_components:
  - source:
      type: local
      path: ../../esphome/components
    components: [satellite1]

satellite1:
  id: satellite1_id
  spi_id: spi_0
  cs_pin: GPIO10
  data_rate: 8000000
  spi_mode: MODE3
  xmos_rst_pin: GPIO4

satellite1_buttons:
  id: button_mgr
  debounce: 20ms
  multi_click_window: 250ms
  buttons:
    - id: test_btn_up
      port: INPUT_A
      pin: 0
      name: "Test Vol+"
      on_single_click:
        - logger.log: "Vol+ clicked"
      on_hold_repeat:
        repeat_interval: 150ms
        then:
          - logger.log: "Vol+ repeat"

    - id: test_btn_action
      gpio: 0
      name: "Test Action"
      on_press:
        - logger.log: "Action pressed"
      on_release:
        - logger.log: "Action released"
      on_single_click:
        - logger.log: "Action single"
      on_double_click:
        - logger.log: "Action double"
      on_triple_click:
        - logger.log: "Action triple"
      on_hold:
        - threshold: 1s
          then:
            - logger.log: "Action hold 1s"
        - threshold: 10s
          then:
            - logger.log: "Action hold 10s"

  combos:
    - buttons: [test_btn_up, test_btn_action]
      hold_duration: 500ms
      on_combo:
        - logger.log: "Combo triggered"
```

**Step 2: Run compile test**

Run: `source .venv/bin/activate && esphome compile tests/components/satellite1_buttons/test_buttons.yaml`

Expected: Compile succeeds (or reveals specific codegen issues to fix).

**Step 3: Fix any compilation errors**

Iterate on the header, cpp, and __init__.py until the test compiles cleanly. Common issues to watch for:
- Trigger wiring — may need to use `btn.get_press_trigger()` directly instead of creating new trigger variables
- Struct initialization in codegen — may need manual `cg.add()` calls instead of `StructInitializer`
- Include paths — verify `#include "../satellite1.h"` resolves correctly

**Step 4: Commit**

```bash
git add tests/components/satellite1_buttons/test_buttons.yaml
git commit -m "test(buttons): add compile test for satellite1_buttons component"
```

---

### Task 5: Iterate until test compiles clean

**Files:**
- Modify: `esphome/components/satellite1/buttons/__init__.py`
- Modify: `esphome/components/satellite1/buttons/satellite1_buttons.h`
- Modify: `esphome/components/satellite1/buttons/satellite1_buttons.cpp`

**Context:**
- The first compile attempt (Task 4, Step 2) will likely reveal issues with the codegen pattern
- Reference `esphome/components/binary_sensor/__init__.py` for the canonical trigger wiring pattern
- Reference `esphome/components/gpio/binary_sensor/__init__.py` for GPIO binary sensor patterns
- Fix errors iteratively: read error, fix, recompile

**Step 1: Run compile, read errors**

Run: `source .venv/bin/activate && esphome compile tests/components/satellite1_buttons/test_buttons.yaml 2>&1 | tail -100`

**Step 2: Fix errors one at a time**

For each error:
1. Read the error message
2. Find the relevant code
3. Fix it
4. Recompile

**Step 3: Verify clean compile**

Run: `source .venv/bin/activate && esphome compile tests/components/satellite1_buttons/test_buttons.yaml`

Expected: `INFO Successfully compiled program.`

**Step 4: Commit**

```bash
git add -A esphome/components/satellite1/buttons/
git commit -m "fix(buttons): resolve compilation issues in satellite1_buttons component"
```

---

### Task 6: Update buttons.yaml to use the new component

**Files:**
- Modify: `config/common/buttons.yaml`
- Modify: `config/satellite1.base.yaml` (if needed for component registration)

**Context:**
- Replace all `binary_sensor` button definitions with `satellite1_buttons` config
- Keep the `event` entity definition (action_button_press_event) — it's used by Home Assistant automations
- Keep the `handle_single_press` script — it contains the voice assistant priority chain
- Remove the `volume_buttons_touched`, `action_button_touched` globals (now managed by ButtonManager)
- Keep `factory_reset_requested` global — it's used by the memory_flasher's `on_erasing_done` callback in `satellite1.base.yaml:166-172`
- The `control_volume` script in `media_player.yaml:264-281` sets `volume_buttons_touched = false` after 2s — this needs to call the manager instead

**Step 1: Rewrite buttons.yaml**

Replace the full content of `config/common/buttons.yaml` with the new satellite1_buttons config. Preserve:
- The `factory_reset_requested` global
- The `action_button_press_event` event entity
- The `handle_single_press` script

Remove:
- `volume_buttons_touched` global
- `action_button_touched` global
- All `binary_sensor` definitions

**Step 2: Update control_volume script in media_player.yaml**

Change `config/common/media_player.yaml:280` from:
```yaml
- lambda: id(volume_buttons_touched) = false;
```
to:
```yaml
- lambda: id(button_manager).set_volume_buttons_touched(false);
```

And change the `lambda: id(volume_buttons_touched) = true;` references in the volume button on_single_click to set it via the manager (or handle this in the button press trigger itself).

**Step 3: Update led_ring.yaml references**

In `config/common/led_ring.yaml:533`, change:
```
id(volume_buttons_touched)
```
to:
```
id(button_manager).volume_buttons_touched()
```

In `config/common/led_ring.yaml:535`, change:
```
id(btn_action).state
```
to a reference to the action button's pressed state from the manager. This may require adding a getter method to Satellite1Button and exposing btn_action via the manager.

**Step 4: Compile the full firmware**

Run: `source .venv/bin/activate && esphome compile config/satellite1.yaml`

Expected: Clean compile.

**Step 5: Commit**

```bash
git add config/common/buttons.yaml config/common/media_player.yaml config/common/led_ring.yaml
git commit -m "refactor(buttons): migrate buttons.yaml to satellite1_buttons component"
```

---

### Task 7: Compile all firmware variants and verify

**Files:**
- No changes — verification only

**Step 1: Compile all three configs**

Run each and verify clean compile:
```bash
source .venv/bin/activate
esphome compile config/satellite1.yaml
esphome compile config/satellite1.ld2410.yaml
esphome compile config/satellite1.ld2450.yaml
```

**Step 2: Run linters**

```bash
source .venv/bin/activate
pre-commit run --all-files
```

Fix any clang-format or yamllint issues.

**Step 3: Commit any lint fixes**

```bash
git add -A
git commit -m "style(buttons): fix formatting from linter"
```

---

### Task 8: Manual testing on hardware

**Files:**
- No changes — testing only

**Step 1: Flash to device**

```bash
source .venv/bin/activate && esphome upload config/satellite1.yaml
```

**Step 2: Stream logs and verify each gesture**

```bash
esphome logs config/satellite1.yaml
```

Test each button:
- [ ] Vol+ single press → volume increases
- [ ] Vol+ hold → volume ramps continuously
- [ ] Vol- single press → volume decreases
- [ ] Vol- hold → volume ramps continuously
- [ ] Mute press → toggles mute
- [ ] Action single press → voice assistant starts (or stops timer/announcement/music)
- [ ] Action double press → event fires, sound plays
- [ ] Action triple press → event fires, sound plays
- [ ] Action long press (1s) → LEDs off, event fires
- [ ] Action 10s hold → factory reset warning
- [ ] Vol+ and Vol- combo → combo triggers
- [ ] LED ring shows volume level on volume button touch
- [ ] LED ring shows action button touched state

**Step 3: Verify Home Assistant integration**

- [ ] Action button events appear in HA
- [ ] Button entities visible in HA device page

---

## Notes for Implementer

### Key ESPHome codegen patterns to reference

- **Trigger wiring**: Look at `esphome/components/binary_sensor/__init__.py` — specifically how `on_press`, `on_release`, `on_click` triggers are built. The pattern is typically:
  ```python
  trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
  await automation.build_automation(trigger, [], conf)
  ```
  where `var` is the parent component. The C++ trigger class constructor takes the parent and registers a callback.

- **Component as platform vs standalone**: This component is registered as `satellite1_buttons:` (standalone), not as `binary_sensor: platform: satellite1_buttons`. This is simpler and matches the `memory_flasher` pattern.

- **The `satellite1_buttons` YAML key**: ESPHome resolves this to `esphome/components/satellite1/buttons/__init__.py` because of the component name. Verify this path resolution works — it may need to be registered differently if ESPHome expects `satellite1_buttons` as a top-level component name vs a sub-component of `satellite1`.

### Potential gotchas

1. **Component name resolution**: ESPHome may not automatically find `satellite1/buttons/` as `satellite1_buttons`. You may need to register it as a separate top-level component or use the `external_components` directive. Test this early.

2. **SPI polling rate**: The current `binary_sensor` GPIO polling happens via ESPHome's default GPIO binary sensor loop. The new manager will poll in its own `loop()`. Verify the polling rate is sufficient (ESPHome's main loop runs at ~16ms intervals by default).

3. **Init guard timing**: The manager starts disabled and should enable when XMOS connects. Wire this via the `on_xmos_connected` trigger in `satellite1.base.yaml`, adding:
   ```yaml
   on_xmos_connected:
     then:
       - lambda: id(button_manager).set_enabled(true);
   ```

4. **The action button (GPIO 0)**: This button is on native ESP32 GPIO, not XMOS SPI. It should work immediately on boot (before XMOS connects). Consider enabling it separately from the SPI buttons, or having `set_enabled()` only gate XMOS buttons.
