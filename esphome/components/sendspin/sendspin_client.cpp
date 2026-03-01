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
  // Pre-build client/hello so ws_handler_ can send it synchronously on connect
  this->stream_.set_hello_message(build_client_hello(get_mac_address_pretty(), App.get_friendly_name()));

  this->stream_.set_on_state_change(
      [this](SendspinStreamState state) { this->defer([this, state]() { this->on_stream_state_changed_(state); }); });
  this->stream_.set_on_clock_synced([this]() { this->clock_synced_.store(true, std::memory_order_relaxed); });
}

void SendspinClient::loop() {
  if (!this->enabled_)
    return;

  // Detect WiFi disconnect — reset so start_server() is re-called on reconnect
  if (this->network_initialized_ && !network::is_connected()) {
    this->network_initialized_ = false;
    this->stream_.stop_server();
  }

  if (!network::is_connected())
    return;

  // Start the server once network is up (or after a reconnect)
  if (!this->network_initialized_) {
    this->network_initialized_ = true;
    this->stream_.start_server();
  }

  // Periodic clock sync — only after hello exchange (READY/STREAMING).
  // Using is_ready() prevents sending client/time before client/hello,
  // which races because state is set atomically from the httpd task
  // but client/hello is sent via a deferred callback.
  if (this->stream_.is_ready()) {
    uint32_t interval = this->clock_synced_.load(std::memory_order_relaxed) ? CLOCK_SYNC_INTERVAL_MS : CLOCK_SYNC_INITIAL_INTERVAL_MS;
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
  this->last_volume_ = volume;
  this->last_muted_ = muted;
  if (this->stream_.is_ready()) {
    this->stream_.send_text(build_client_state(volume, muted));
  }
}

void SendspinClient::do_clock_sync_() {
  int64_t now_us = esp_timer_get_time();
  this->stream_.send_text(build_client_time(now_us));
  this->last_clock_sync_ms_ = millis();
}

void SendspinClient::on_stream_state_changed_(SendspinStreamState state) {
  ESP_LOGD(TAG, "Stream state -> %d", static_cast<int>(state));

  if (state == SendspinStreamState::CONNECTED) {
    // client/hello is already sent synchronously from ws_handler_ — just reset clock state
    this->clock_synced_.store(false, std::memory_order_relaxed);
    this->last_clock_sync_ms_ = 0;
  } else if (state == SendspinStreamState::READY) {
    this->clock_synced_.store(false, std::memory_order_relaxed);
    this->stream_.send_text(build_client_state(this->last_volume_, this->last_muted_));
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
