#include "sendspin_client.h"

#include "esphome/components/network/util.h"
#include "esphome/components/speaker/media_player/speaker_media_player.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "esp_timer.h"

namespace esphome {
namespace sendspin {

static const char *const TAG = "sendspin_client";

void SendspinClient::setup() {
  this->stream_.set_on_state_change(
      [this](SendspinStreamState state) { this->defer([this, state]() { this->on_stream_state_changed_(state); }); });
}

void SendspinClient::loop() {
  if (!this->enabled_)
    return;

  // Start the server once network is up
  if (!this->network_initialized_ && network::is_connected()) {
    this->network_initialized_ = true;
    this->stream_.start_server();
  }

  if (!network::is_connected())
    return;

  // Periodic clock sync while connected
  if (this->stream_.is_connected()) {
    uint32_t interval = this->clock_synced_ ? CLOCK_SYNC_INTERVAL_MS : CLOCK_SYNC_INITIAL_INTERVAL_MS;
    if (millis() - this->last_clock_sync_ms_ >= interval) {
      this->do_clock_sync_();
    }
  }

  // Flush one outbound controller command per loop iteration
  this->controller_.flush_one();
}

void SendspinClient::enable() {
  this->enabled_ = true;
  if (network::is_connected() && !this->stream_.is_connected()) {
    this->stream_.start_server();
  }
}

void SendspinClient::disable() {
  this->enabled_ = false;
  this->stream_.stop_server();
}

void SendspinClient::report_volume(float volume, bool muted) {
  if (this->stream_.is_connected()) {
    this->stream_.send_text(build_client_state(volume, muted));
  }
}

void SendspinClient::do_clock_sync_() {
  int64_t now_us = esp_timer_get_time();
  this->stream_.send_text(build_client_time(now_us));
  this->last_clock_sync_ms_ = millis();
  this->clock_synced_ = true;
}

void SendspinClient::on_stream_state_changed_(SendspinStreamState state) {
  ESP_LOGD(TAG, "Stream state -> %d", static_cast<int>(state));

  if (state == SendspinStreamState::CONNECTED) {
    // Send client/hello immediately after WS handshake
    std::string hello = build_client_hello(get_mac_address_pretty(), App.get_friendly_name());
    this->stream_.send_text(hello);
    this->clock_synced_ = false;
    this->last_clock_sync_ms_ = 0;
  } else if (state == SendspinStreamState::READY) {
    this->clock_synced_ = false;
  } else if (state == SendspinStreamState::STREAMING) {
    // Trigger media pipeline via the media player
    if (this->media_player_ != nullptr) {
      this->media_player_->play_sendspin_stream();
    }
  } else if (state == SendspinStreamState::IDLE) {
    // MA disconnected — stop media pipeline
    if (this->media_player_ != nullptr) {
      this->media_player_->make_call()
          .set_command(media_player::MediaPlayerCommand::MEDIA_PLAYER_COMMAND_STOP)
          .perform();
    }
  }
}

}  // namespace sendspin
}  // namespace esphome
