#pragma once

#include "messages.h"
#include "sendspin_stream.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <string>

namespace esphome {
namespace sendspin {

class SendspinController {
 public:
  SendspinController() { this->queue_ = xQueueCreate(8, sizeof(char *)); }
  ~SendspinController() {
    if (this->queue_ != nullptr) {
      char *msg = nullptr;
      while (xQueueReceive(this->queue_, &msg, 0) == pdTRUE)
        delete[] msg;
      vQueueDelete(this->queue_);
    }
  }

  void set_stream(SendspinStream *stream) { this->stream_ = stream; }

  /// Queue a play/pause toggle command
  void play_pause() { this->enqueue_(build_controller_command("playpause")); }

  /// Queue a next-track command
  void next() { this->enqueue_(build_controller_command("next")); }

  /// Queue a previous-track command
  void previous() { this->enqueue_(build_controller_command("previous")); }

  /// Queue a volume set command (0.0–1.0)
  void set_volume(float volume) { this->enqueue_(build_set_volume_command(volume)); }

  /// Flush up to one queued command per call — call from loop()
  void flush_one();

 protected:
  void enqueue_(const std::string &msg);

  SendspinStream *stream_{nullptr};
  QueueHandle_t queue_{nullptr};
};

}  // namespace sendspin
}  // namespace esphome
