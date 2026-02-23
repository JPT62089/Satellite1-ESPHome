#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/audio/chunked_ring_buffer.h"

#include "esp_http_server.h"
#include "esp_err.h"

namespace esphome {
namespace sendspin {

enum class SendspinStreamState {
  IDLE,       // Server not started or no client connected
  CONNECTED,  // WebSocket handshake complete, exchanging hello
  READY,      // server/hello received, clock sync in progress
  STREAMING,  // stream/start received, audio flowing
  ERROR,
};

class SendspinStream {
 public:
  explicit SendspinStream(uint16_t port) : port_(port) {}

  /// Start the WebSocket server and register mDNS service
  esp_err_t start_server();

  /// Stop the WebSocket server and unregister mDNS
  void stop_server();

  /// Called by AudioReader to begin receiving audio into ring_buffer.
  /// notification_task will be notified (via xTaskNotify) when audio chunks arrive.
  esp_err_t start_with_notify(std::weak_ptr<audio::TimedRingBuffer> ring_buffer, TaskHandle_t notification_task);

  /// Called by AudioReader to stop the audio stream
  esp_err_t stop_streaming();

  /// Send a text frame to the connected MA (thread-safe; callable from any task)
  esp_err_t send_text(const std::string &msg);

  bool is_connected() const {
    auto s = state_.load(std::memory_order_relaxed);
    return s == SendspinStreamState::CONNECTED || s == SendspinStreamState::READY ||
           s == SendspinStreamState::STREAMING;
  }
  bool is_streaming() const { return state_.load(std::memory_order_relaxed) == SendspinStreamState::STREAMING; }
  bool is_idle() const { return state_.load(std::memory_order_relaxed) == SendspinStreamState::IDLE; }

  /// Set callback invoked (on main task via defer) when state changes.
  /// IMPORTANT: must only be called once (from setup()), before the WS server starts.
  /// The callback is read from the httpd task without synchronization.
  void set_on_state_change(std::function<void(SendspinStreamState)> cb) { this->on_state_change_ = std::move(cb); }

  /// Set callback invoked from the httpd task each time a clock-sync round-trip completes.
  /// Callers must ensure the callback is safe to invoke from any FreeRTOS task.
  void set_on_clock_synced(std::function<void()> cb) { this->on_clock_synced_ = std::move(cb); }

  /// Convert a server-clock microsecond timestamp to local esp_timer_get_time() microseconds
  int64_t server_to_local_us(int64_t server_us) const {
    return server_us - this->clock_offset_us_.load(std::memory_order_relaxed);
  }

  /// Update clock offset from a server/time response
  void update_clock_offset(int64_t client_transmitted, int64_t server_received, int64_t server_transmitted,
                           int64_t client_received);

 protected:
  /// HTTP server WebSocket URI handler (static, called by esp_http_server)
  static esp_err_t ws_handler_(httpd_req_t *req);

  /// Process an incoming text frame (JSON) from MA
  void handle_text_frame_(const std::string &json_str);

  /// Process an incoming binary frame (audio data) from MA
  void handle_binary_frame_(const uint8_t *data, size_t len);

  void set_state_(SendspinStreamState state);

  uint16_t port_;
  httpd_handle_t httpd_{nullptr};
  std::atomic<int> connected_fd_{-1};  // socket fd of the connected MA client

  std::atomic<SendspinStreamState> state_{SendspinStreamState::IDLE};
  std::atomic<int64_t> clock_offset_us_{0};  // server_clock - local_clock (microseconds)
  bool codec_header_sent_{false};

  std::weak_ptr<audio::TimedRingBuffer> write_ring_buffer_;
  TaskHandle_t notification_target_{nullptr};

  std::function<void(SendspinStreamState)> on_state_change_;
  std::function<void()> on_clock_synced_;
};

}  // namespace sendspin
}  // namespace esphome
