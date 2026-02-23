#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#include "sendspin_controller.h"
#include "sendspin_stream.h"

namespace esphome {
namespace speaker {
class SpeakerMediaPlayer;
}  // namespace speaker
namespace sendspin {

class SendspinClient : public Component {
 public:
  explicit SendspinClient(uint16_t port = 8928) : stream_(port) { controller_.set_stream(&stream_); }

  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION + 5; }
  void setup() override;
  void loop() override;

  void set_media_player(esphome::speaker::SpeakerMediaPlayer *mp) { this->media_player_ = mp; }

  void enable();
  void disable();

  SendspinStream *get_stream() { return &this->stream_; }

  // ESPHome actions — callable from YAML automations
  void action_play_pause() { this->controller_.play_pause(); }
  void action_next() { this->controller_.next(); }
  void action_previous() { this->controller_.previous(); }
  void action_set_volume(float v) { this->controller_.set_volume(v); }

  void report_volume(float volume, bool muted);

 protected:
  void on_stream_state_changed_(SendspinStreamState state);
  void do_clock_sync_();

  bool enabled_{true};
  bool network_initialized_{false};

  SendspinStream stream_;
  SendspinController controller_;

  esphome::speaker::SpeakerMediaPlayer *media_player_{nullptr};

  uint32_t last_clock_sync_ms_{0};
  static constexpr uint32_t CLOCK_SYNC_INTERVAL_MS = 2000;
  static constexpr uint32_t CLOCK_SYNC_INITIAL_INTERVAL_MS = 100;
  bool clock_synced_{false};
};

}  // namespace sendspin
}  // namespace esphome
