#include "satellite1_buttons.h"
#include "esphome/core/log.h"

namespace esphome {
namespace satellite1 {

// --- Satellite1Button ---

void Satellite1Button::process(bool raw_pressed, uint32_t now_ms) {
  // --- Debounce filter ---
  if (raw_pressed != this->last_raw_) {
    this->last_raw_ = raw_pressed;
    this->last_raw_change_ms_ = now_ms;
    this->debounce_pending_ = true;
  }

  if (this->debounce_pending_) {
    if ((now_ms - this->last_raw_change_ms_) < this->debounce_ms_) {
      return;  // still settling
    }
    this->debounce_pending_ = false;
  }

  bool new_pressed = this->last_raw_;

  // --- Edge detection ---
  if (new_pressed && !this->debounced_pressed_) {
    // Rising edge: press
    this->debounced_pressed_ = true;
    this->press_start_ms_ = now_ms;
    this->last_repeat_ms_ = now_ms;
    this->hold_consumed_ = false;

    // Reset hold thresholds
    for (auto &ht : this->hold_thresholds_) {
      ht.fired = false;
    }

    this->press_trigger_.trigger();

  } else if (!new_pressed && this->debounced_pressed_) {
    // Falling edge: release
    this->debounced_pressed_ = false;
    this->release_trigger_.trigger();

    // Count clicks for multi-click detection (skip if a hold threshold fired)
    if (!this->hold_consumed_) {
      this->click_count_++;
      this->last_release_ms_ = now_ms;
    }
  }

  // --- While held: progressive hold thresholds ---
  if (this->debounced_pressed_) {
    uint32_t held_ms = now_ms - this->press_start_ms_;

    // Check thresholds and fire any newly met
    for (auto &ht : this->hold_thresholds_) {
      if (!ht.fired && held_ms >= ht.duration_ms) {
        ht.fired = true;
        this->hold_consumed_ = true;
        ht.trigger->trigger();
      }
    }

    // Hold-to-repeat
    if (this->repeat_interval_ms_ > 0 && held_ms >= this->repeat_interval_ms_) {
      if ((now_ms - this->last_repeat_ms_) >= this->repeat_interval_ms_) {
        this->last_repeat_ms_ = now_ms;
        this->hold_repeat_trigger_.trigger();
      }
    }
  }

  // --- Multi-click window expiry (only when not pressed) ---
  if (!this->debounced_pressed_ && this->click_count_ > 0) {
    if ((now_ms - this->last_release_ms_) >= this->multi_click_window_ms_) {
      switch (this->click_count_) {
        case 1:
          this->single_click_trigger_.trigger();
          break;
        case 2:
          this->double_click_trigger_.trigger();
          break;
        default:
          // 3 or more
          this->triple_click_trigger_.trigger();
          break;
      }
      this->click_count_ = 0;
    }
  }
}

// --- Satellite1ButtonManager ---

void Satellite1ButtonManager::setup() {
  ESP_LOGCONFIG(BUTTONS_TAG, "Setting up Satellite1 Button Manager...");
  for (auto *btn : this->buttons_) {
    if (btn->is_native()) {
      // Native GPIO pins are set up by ESPHome pin infrastructure
      ESP_LOGD(BUTTONS_TAG, "  Native GPIO button: %s", btn->get_name().c_str());
    } else {
      ESP_LOGD(BUTTONS_TAG, "  SPI button: %s", btn->get_name().c_str());
    }
  }
}

void Satellite1ButtonManager::loop() {
  if (!this->enabled_)
    return;

  uint32_t now_ms = millis();

  // Single SPI poll for all XMOS buttons
  bool spi_ok = this->parent_->request_status_register_update();
  uint8_t port_a = 0;
  if (spi_ok) {
    port_a = this->parent_->get_dc_status(DC_STATUS_REGISTER::GPIO_PORT_IN_A);
  }

  for (auto *btn : this->buttons_) {
    bool raw;
    if (btn->is_native()) {
      raw = btn->read_native_gpio();
    } else {
      if (!spi_ok)
        continue;  // skip SPI buttons if poll failed
      raw = btn->read_from_port(port_a);
    }
    btn->process(raw, now_ms);
  }

  this->check_combos_(now_ms);
}

void Satellite1ButtonManager::check_combos_(uint32_t now_ms) {
  for (auto *combo : this->combos_) {
    bool all_pressed = true;
    for (auto *btn : combo->get_buttons()) {
      if (!btn->is_pressed()) {
        all_pressed = false;
        break;
      }
    }

    if (all_pressed) {
      if (combo->get_all_pressed_since_ms() == 0) {
        combo->set_all_pressed_since_ms(now_ms);
      }
      if (!combo->is_fired() && (now_ms - combo->get_all_pressed_since_ms()) >= combo->get_hold_duration_ms()) {
        combo->set_fired(true);
        combo->get_trigger()->trigger();
      }
    } else {
      combo->set_all_pressed_since_ms(0);
      combo->set_fired(false);
    }
  }
}

void Satellite1ButtonManager::dump_config() {
  ESP_LOGCONFIG(BUTTONS_TAG, "Satellite1 Button Manager:");
  ESP_LOGCONFIG(BUTTONS_TAG, "  Buttons: %u", this->buttons_.size());
  for (auto *btn : this->buttons_) {
    ESP_LOGCONFIG(BUTTONS_TAG, "    - %s (native: %s)", btn->get_name().c_str(), YESNO(btn->is_native()));
  }
  ESP_LOGCONFIG(BUTTONS_TAG, "  Combos: %u", this->combos_.size());
}

}  // namespace satellite1
}  // namespace esphome
