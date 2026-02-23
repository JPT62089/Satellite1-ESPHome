#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/gpio.h"
#include "esphome/core/hal.h"

#include "esphome/components/satellite1/satellite1.h"
#include "esphome/components/satellite1/sat_gpio.h"

#include <string>
#include <vector>

namespace esphome {
namespace satellite1 {

static const char *const BUTTONS_TAG = "satellite1.buttons";

struct HoldThreshold {
  uint32_t duration_ms;
  Trigger<> *trigger;
  bool fired;
};

class Satellite1Button {
 public:
  // Configuration setters
  void set_name(const std::string &name) { this->name_ = name; }
  void set_port(XMOSPort port) { this->port_ = port; }
  void set_pin(uint8_t pin) { this->pin_ = pin; }
  void set_native_gpio(InternalGPIOPin *gpio) { this->native_gpio_ = gpio; }
  void set_debounce_ms(uint32_t ms) { this->debounce_ms_ = ms; }
  void set_multi_click_window_ms(uint32_t ms) { this->multi_click_window_ms_ = ms; }
  void set_repeat_interval_ms(uint32_t ms) { this->repeat_interval_ms_ = ms; }
  void set_icon(const std::string &icon) { this->icon_ = icon; }

  // Trigger accessors — owned by this object, allocated in constructor
  Trigger<> *get_press_trigger() { return &this->press_trigger_; }
  Trigger<> *get_release_trigger() { return &this->release_trigger_; }
  Trigger<> *get_single_click_trigger() { return &this->single_click_trigger_; }
  Trigger<> *get_double_click_trigger() { return &this->double_click_trigger_; }
  Trigger<> *get_triple_click_trigger() { return &this->triple_click_trigger_; }
  Trigger<> *get_hold_repeat_trigger() { return &this->hold_repeat_trigger_; }

  void add_hold_threshold(uint32_t duration_ms, Trigger<> *trigger) {
    this->hold_thresholds_.push_back({duration_ms, trigger, false});
  }

  // State machine
  void process(bool raw_pressed, uint32_t now_ms);

  // State queries
  bool is_pressed() const { return this->debounced_pressed_; }
  const std::string &get_name() const { return this->name_; }

  // Reading helpers
  bool read_from_port(uint8_t port_value) const { return (port_value & (1 << this->pin_)) != 0; }
  bool read_native_gpio() const { return this->native_gpio_ != nullptr ? this->native_gpio_->digital_read() : false; }
  bool is_native() const { return this->native_gpio_ != nullptr; }

 protected:
  // Config
  std::string name_;
  std::string icon_;
  XMOSPort port_{XMOSPort::INPUT_A};
  uint8_t pin_{0};
  InternalGPIOPin *native_gpio_{nullptr};
  uint32_t debounce_ms_{20};
  uint32_t multi_click_window_ms_{300};
  uint32_t repeat_interval_ms_{0};  // 0 = disabled

  // State
  bool debounced_pressed_{false};
  bool last_raw_{false};
  uint32_t last_raw_change_ms_{0};
  bool debounce_pending_{false};

  // Edge tracking
  uint32_t press_start_ms_{0};

  // Multi-click state
  uint8_t click_count_{0};
  uint32_t last_release_ms_{0};

  // Hold-to-repeat state
  uint32_t last_repeat_ms_{0};

  // Triggers
  Trigger<> press_trigger_;
  Trigger<> release_trigger_;
  Trigger<> single_click_trigger_;
  Trigger<> double_click_trigger_;
  Trigger<> triple_click_trigger_;
  Trigger<> hold_repeat_trigger_;

  // Progressive hold thresholds
  std::vector<HoldThreshold> hold_thresholds_;
};

class ComboConfig {
 public:
  void add_button(Satellite1Button *button) { this->buttons_.push_back(button); }
  void set_hold_duration_ms(uint32_t ms) { this->hold_duration_ms_ = ms; }
  void set_trigger(Trigger<> *trigger) { this->trigger_ = trigger; }

  std::vector<Satellite1Button *> &get_buttons() { return this->buttons_; }
  uint32_t get_hold_duration_ms() const { return this->hold_duration_ms_; }
  Trigger<> *get_trigger() { return this->trigger_; }
  bool is_fired() const { return this->fired_; }
  void set_fired(bool fired) { this->fired_ = fired; }
  uint32_t get_all_pressed_since_ms() const { return this->all_pressed_since_ms_; }
  void set_all_pressed_since_ms(uint32_t ms) { this->all_pressed_since_ms_ = ms; }

 protected:
  std::vector<Satellite1Button *> buttons_;
  uint32_t hold_duration_ms_{0};
  Trigger<> *trigger_{nullptr};
  bool fired_{false};
  uint32_t all_pressed_since_ms_{0};
};

class Satellite1ButtonManager : public Component, public Satellite1SPIService {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void add_button(Satellite1Button *button) { this->buttons_.push_back(button); }
  void add_combo(ComboConfig *combo) { this->combos_.push_back(combo); }

  void set_enabled(bool enabled) { this->enabled_ = enabled; }
  bool get_volume_buttons_touched() const { return this->volume_buttons_touched_; }
  void set_volume_buttons_touched(bool touched) { this->volume_buttons_touched_ = touched; }

 protected:
  void check_combos_(uint32_t now_ms);

  std::vector<Satellite1Button *> buttons_;
  std::vector<ComboConfig *> combos_;
  bool enabled_{true};
  bool volume_buttons_touched_{false};
};

}  // namespace satellite1
}  // namespace esphome
