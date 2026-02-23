#include "sendspin_stream.h"
#include "messages.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include "mdns.h"
#include "esp_timer.h"

namespace esphome {
namespace sendspin {

static const char *const TAG = "sendspin_stream";

esp_err_t SendspinStream::start_server() {
  if (this->httpd_ != nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  // Register mDNS service so MA can discover this device
  esp_err_t err = mdns_service_add(nullptr, "_sendspin", "_tcp", this->port_, nullptr, 0);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mdns_service_add failed: %s (may already be registered)", esp_err_to_name(err));
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = this->port_;
  config.lru_purge_enable = true;

  err = httpd_start(&this->httpd_, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
    return err;
  }

  const httpd_uri_t ws_uri = {
      .uri = "/sendspin",
      .method = HTTP_GET,
      .handler = SendspinStream::ws_handler_,
      .user_ctx = this,
      .is_websocket = true,
      .handle_ws_control_frames = false,
      .supported_subprotocol = nullptr,
  };

  err = httpd_register_uri_handler(this->httpd_, &ws_uri);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register WebSocket URI handler: %s", esp_err_to_name(err));
    httpd_stop(this->httpd_);
    this->httpd_ = nullptr;
    return err;
  }

  ESP_LOGI(TAG, "Sendspin WebSocket server started on port %d", this->port_);
  return ESP_OK;
}

void SendspinStream::stop_server() {
  if (this->httpd_) {
    this->send_text(build_client_goodbye());
    httpd_stop(this->httpd_);
    this->httpd_ = nullptr;
    this->connected_fd_.store(-1, std::memory_order_relaxed);
  }
  mdns_service_remove("_sendspin", "_tcp");
  this->set_state_(SendspinStreamState::IDLE);
}

esp_err_t SendspinStream::start_with_notify(std::weak_ptr<audio::TimedRingBuffer> ring_buffer,
                                            TaskHandle_t notification_task) {
  this->write_ring_buffer_ = ring_buffer;
  this->notification_target_ = notification_task;
  this->codec_header_sent_ = false;
  auto rb = ring_buffer.lock();
  if (rb)
    rb->reset();
  return ESP_OK;
}

esp_err_t SendspinStream::stop_streaming() {
  this->notification_target_ = nullptr;
  this->write_ring_buffer_.reset();
  if (this->is_connected())
    this->set_state_(SendspinStreamState::READY);
  return ESP_OK;
}

esp_err_t SendspinStream::send_text(const std::string &msg) {
  if (this->httpd_ == nullptr || this->connected_fd_.load(std::memory_order_relaxed) < 0)
    return ESP_ERR_INVALID_STATE;

  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.payload = reinterpret_cast<uint8_t *>(const_cast<char *>(msg.c_str()));
  frame.len = msg.size();
  // httpd_ws_send_frame_async is thread-safe: safe to call from any task
  return httpd_ws_send_frame_async(this->httpd_, this->connected_fd_.load(std::memory_order_relaxed), &frame);
}

void SendspinStream::update_clock_offset(int64_t client_transmitted, int64_t server_received,
                                         int64_t server_transmitted, int64_t client_received) {
  // NTP-style: offset = ((server_received - client_transmitted) + (server_transmitted - client_received)) / 2
  // offset > 0 means server clock is ahead of local clock
  int64_t offset = ((server_received - client_transmitted) + (server_transmitted - client_received)) / 2;
  this->clock_offset_us_.store(offset, std::memory_order_relaxed);
  ESP_LOGV(TAG, "Clock offset updated: %" PRId64 " us", offset);
  if (this->on_clock_synced_)
    this->on_clock_synced_();
}

// Static WebSocket handler — called by esp_http_server for each frame
esp_err_t SendspinStream::ws_handler_(httpd_req_t *req) {
  auto *self = static_cast<SendspinStream *>(req->user_ctx);

  if (req->method == HTTP_GET) {
    // New WebSocket connection established
    self->connected_fd_.store(httpd_req_to_sockfd(req), std::memory_order_relaxed);
    self->codec_header_sent_ = false;
    ESP_LOGI(TAG, "MA connected (fd=%d)", self->connected_fd_.load(std::memory_order_relaxed));
    self->set_state_(SendspinStreamState::CONNECTED);
    return ESP_OK;
  }

  // Read the frame header to determine size and type
  httpd_ws_frame_t frame = {};
  esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
  if (err != ESP_OK)
    return err;

  if (frame.len == 0)
    return ESP_OK;

  // Allocate and read the full frame payload
  frame.payload = new (std::nothrow) uint8_t[frame.len];
  if (!frame.payload)
    return ESP_ERR_NO_MEM;

  err = httpd_ws_recv_frame(req, &frame, frame.len);
  if (err != ESP_OK) {
    delete[] frame.payload;
    return err;
  }

  if (frame.type == HTTPD_WS_TYPE_TEXT) {
    std::string json_str(reinterpret_cast<char *>(frame.payload), frame.len);
    self->handle_text_frame_(json_str);
  } else if (frame.type == HTTPD_WS_TYPE_BINARY) {
    self->handle_binary_frame_(frame.payload, frame.len);
  } else if (frame.type == HTTPD_WS_TYPE_CLOSE) {
    ESP_LOGI(TAG, "MA disconnected");
    self->connected_fd_.store(-1, std::memory_order_relaxed);
    self->set_state_(SendspinStreamState::IDLE);
  }

  delete[] frame.payload;
  return ESP_OK;
}

void SendspinStream::handle_text_frame_(const std::string &json_str) {
  ESP_LOGV(TAG, "RX text: %s", json_str.c_str());

  if (is_server_hello(json_str)) {
    ESP_LOGI(TAG, "server/hello received");
    this->set_state_(SendspinStreamState::READY);
    return;
  }

  ClockSyncResponse sync;
  if (parse_server_time(json_str, sync)) {
    int64_t client_received = esp_timer_get_time();
    this->update_clock_offset(sync.client_transmitted, sync.server_received, sync.server_transmitted, client_received);
    return;
  }

  StreamStartInfo stream_info;
  if (parse_stream_start(json_str, stream_info)) {
    ESP_LOGI(TAG, "stream/start: codec=%s %dHz %dch", stream_info.codec.c_str(), stream_info.sample_rate,
             stream_info.channels);
    this->set_state_(SendspinStreamState::STREAMING);
    return;
  }
}

void SendspinStream::handle_binary_frame_(const uint8_t *data, size_t len) {
  if (this->state_.load(std::memory_order_relaxed) != SendspinStreamState::STREAMING)
    return;

  if (len < BINARY_HEADER_SIZE)
    return;

  uint8_t type_byte = data[0];
  if (type_byte < 4 || type_byte > 7)
    return;  // Not an audio frame

  int64_t server_ts_us = parse_binary_timestamp(data);
  int64_t local_ts_us = this->server_to_local_us(server_ts_us);

  const uint8_t *audio_data = data + BINARY_HEADER_SIZE;
  size_t audio_len = len - BINARY_HEADER_SIZE;

  if (audio_len == 0)
    return;

  auto rb = this->write_ring_buffer_.lock();
  if (!rb)
    return;

  // Convert the local microsecond timestamp to tv_t (sec + usec components)
  audio::tv_t stamp = audio::tv_t::from_microseconds(local_ts_us);

  // Skip chunks that are already in the past
  if (stamp < audio::tv_t::now()) {
    ESP_LOGV(TAG, "Dropping stale audio chunk (%" PRId64 " us behind)",
             audio::tv_t::now().to_microseconds() - local_ts_us);
    return;
  }

  // Acquire a write slot in the timed ring buffer large enough for the header + audio payload
  audio::timed_chunk_t *timed_chunk = nullptr;
  rb->acquire_write_chunk(&timed_chunk, sizeof(audio::timed_chunk_t) + audio_len, pdMS_TO_TICKS(10));
  if (timed_chunk == nullptr) {
    ESP_LOGW(TAG, "Failed to acquire ring buffer write chunk (dropped %u bytes)", (unsigned) audio_len);
    return;
  }

  // Fill the chunk: timestamp first, then copy the raw audio bytes into the flexible array
  timed_chunk->stamp = stamp;
  std::memcpy(timed_chunk->data, audio_data, audio_len);

  rb->release_write_chunk(timed_chunk, audio_len);

  // Wake the AudioReader task so it processes the new data promptly
  if (this->notification_target_ != nullptr) {
    xTaskNotify(this->notification_target_, static_cast<uint32_t>(this->state_.load(std::memory_order_relaxed)),
                eSetValueWithOverwrite);
  }
}

void SendspinStream::set_state_(SendspinStreamState state) {
  this->state_.store(state, std::memory_order_relaxed);
  if (this->on_state_change_) {
    auto cb = this->on_state_change_;
    cb(state);
  }
}

}  // namespace sendspin
}  // namespace esphome
