#include "sendspin_controller.h"

#include "esphome/core/log.h"

#include <cstring>

namespace esphome {
namespace sendspin {

static const char *const TAG = "sendspin_controller";

void SendspinController::enqueue_(const std::string &msg) {
  char *copy = new (std::nothrow) char[msg.size() + 1];
  if (copy == nullptr) {
    ESP_LOGW(TAG, "Out of memory, dropping command");
    return;
  }
  std::memcpy(copy, msg.c_str(), msg.size() + 1);
  if (xQueueSend(this->queue_, &copy, 0) != pdTRUE) {
    ESP_LOGW(TAG, "Controller queue full, dropping command");
    delete[] copy;
  }
}

void SendspinController::flush_one() {
  if (this->stream_ == nullptr || !this->stream_->is_ready())
    return;
  char *msg = nullptr;
  if (xQueueReceive(this->queue_, &msg, 0) == pdTRUE) {
    this->stream_->send_text(std::string(msg));
    delete[] msg;
  }
}

}  // namespace sendspin
}  // namespace esphome
