#pragma once

#include "esphome/components/json/json_util.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"

#include <string>

namespace esphome {
namespace sendspin {

// Binary frame type byte (first byte of every binary WS frame)
enum class BinaryFrameType : uint8_t {
  AUDIO_CHANNEL_0 = 4,  // primary audio stream
  AUDIO_CHANNEL_1 = 5,
  AUDIO_CHANNEL_2 = 6,
  AUDIO_CHANNEL_3 = 7,
};

// Binary frame header: 1 byte type + 8 bytes big-endian int64 server-microsecond timestamp
static constexpr size_t BINARY_HEADER_SIZE = 9;

inline int64_t parse_binary_timestamp(const uint8_t *data) {
  int64_t ts = 0;
  for (int i = 0; i < 8; i++) {
    ts = (ts << 8) | data[1 + i];
  }
  return ts;
}

// Outbound: client/hello — sent immediately after WS connection is established
inline std::string build_client_hello(const std::string &client_id, const std::string &device_name) {
  return json::build_json([&](JsonObject root) {
    root["type"] = "client/hello";
    JsonObject data = root["data"].to<JsonObject>();
    data["client_id"] = client_id;
    data["name"] = device_name;
    data["version"] = 1;

    JsonArray roles = data["supported_roles"].to<JsonArray>();
    JsonObject player_role = roles.add<JsonObject>();
    player_role["role"] = "player@v1";
    JsonObject player_support = player_role["player_support"].to<JsonObject>();
    JsonArray formats = player_support["supported_formats"].to<JsonArray>();
    JsonObject fmt = formats.add<JsonObject>();
    fmt["codec"] = "flac";
    fmt["channels"] = 2;
    fmt["sample_rate"] = 48000;
    fmt["bit_depth"] = 16;
    player_support["buffer_capacity"] = 131072;  // 128 KB

    JsonObject ctrl_role = roles.add<JsonObject>();
    ctrl_role["role"] = "controller@v1";
  });
}

// Outbound: client/time — clock sync request, ts_us is esp_timer_get_time() value
inline std::string build_client_time(int64_t ts_us) {
  return json::build_json([&](JsonObject root) {
    root["type"] = "client/time";
    root["data"]["client_transmitted"] = ts_us;
  });
}

// Outbound: client/state — reports player volume and sync state
inline std::string build_client_state(float volume, bool muted) {
  return json::build_json([&](JsonObject root) {
    root["type"] = "client/state";
    JsonObject data = root["data"].to<JsonObject>();
    data["state"] = "synchronized";
    JsonObject player = data["player"].to<JsonObject>();
    player["volume"] = static_cast<int>(volume * 100.0f);
    player["muted"] = muted;
  });
}

// Outbound: controller commands (play/pause, next, previous, set_volume)
inline std::string build_controller_command(const std::string &command) {
  return json::build_json([&](JsonObject root) {
    root["type"] = "server/command";  // client sends this to request a command
    root["data"]["command"] = command;
  });
}

inline std::string build_set_volume_command(float volume) {
  return json::build_json([&](JsonObject root) {
    root["type"] = "server/command";
    JsonObject data = root["data"].to<JsonObject>();
    data["command"] = "volume";
    data["volume"] = static_cast<int>(volume * 100.0f);
  });
}

// Outbound: client/goodbye
inline std::string build_client_goodbye() {
  return json::build_json([](JsonObject root) { root["type"] = "client/goodbye"; });
}

struct StreamStartInfo {
  std::string codec;  // "flac", "opus", "pcm"
  int sample_rate{48000};
  int channels{2};
  int bit_depth{16};
  int64_t start_ts_us{0};  // server-clock microseconds when stream starts
  bool valid{false};
};

inline bool parse_stream_start(const std::string &json_str, StreamStartInfo &out) {
  return json::parse_json(json_str, [&](JsonObject root) -> bool {
    if (!root["type"].is<std::string>() || root["type"].as<std::string>() != "stream/start")
      return false;
    if (!root["data"].is<JsonObject>())
      return false;
    JsonObject data = root["data"].as<JsonObject>();
    if (!data["codec"].is<std::string>())
      return false;
    out.codec = data["codec"].as<std::string>();
    if (data["sample_rate"].is<int>())
      out.sample_rate = data["sample_rate"].as<int>();
    if (data["channels"].is<int>())
      out.channels = data["channels"].as<int>();
    if (data["bit_depth"].is<int>())
      out.bit_depth = data["bit_depth"].as<int>();
    if (data["start_ts_us"].is<int64_t>())
      out.start_ts_us = data["start_ts_us"].as<int64_t>();
    out.valid = true;
    return true;
  });
}

struct ClockSyncResponse {
  int64_t client_transmitted{0};
  int64_t server_received{0};
  int64_t server_transmitted{0};
  bool valid{false};
};

inline bool parse_server_time(const std::string &json_str, ClockSyncResponse &out) {
  return json::parse_json(json_str, [&](JsonObject root) -> bool {
    if (!root["type"].is<std::string>() || root["type"].as<std::string>() != "server/time")
      return false;
    if (!root["data"].is<JsonObject>())
      return false;
    JsonObject data = root["data"].as<JsonObject>();
    if (!data["client_transmitted"].is<int64_t>() || !data["server_received"].is<int64_t>() ||
        !data["server_transmitted"].is<int64_t>())
      return false;
    out.client_transmitted = data["client_transmitted"].as<int64_t>();
    out.server_received = data["server_received"].as<int64_t>();
    out.server_transmitted = data["server_transmitted"].as<int64_t>();
    out.valid = true;
    return true;
  });
}

// Returns true if the message type is "server/hello"
inline bool is_server_hello(const std::string &json_str) {
  bool result = false;
  json::parse_json(json_str, [&](JsonObject root) -> bool {
    result = root["type"].is<std::string>() && root["type"].as<std::string>() == "server/hello";
    return true;
  });
  return result;
}

}  // namespace sendspin
}  // namespace esphome
