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
    JsonObject payload = root["payload"].to<JsonObject>();
    payload["client_id"] = client_id;
    payload["name"] = device_name;
    payload["version"] = 1;

    JsonArray roles = payload["supported_roles"].to<JsonArray>();
    roles.add("player@v1");
    roles.add("controller@v1");

    JsonObject player_support = payload["player@v1_support"].to<JsonObject>();
    JsonArray formats = player_support["supported_formats"].to<JsonArray>();
    JsonObject fmt = formats.add<JsonObject>();
    fmt["codec"] = "flac";
    fmt["channels"] = 2;
    fmt["sample_rate"] = 48000;
    fmt["bit_depth"] = 16;
    player_support["buffer_capacity"] = 131072;  // 128 KB
    JsonArray commands = player_support["supported_commands"].to<JsonArray>();
    commands.add("volume");
    commands.add("mute");
  });
}

// Outbound: client/time — clock sync request, ts_us is esp_timer_get_time() value
inline std::string build_client_time(int64_t ts_us) {
  return json::build_json([&](JsonObject root) {
    root["type"] = "client/time";
    root["payload"]["client_transmitted"] = ts_us;
  });
}

// Outbound: client/state — reports player volume and sync state
inline std::string build_client_state(float volume, bool muted) {
  return json::build_json([&](JsonObject root) {
    root["type"] = "client/state";
    JsonObject payload = root["payload"].to<JsonObject>();
    payload["state"] = "synchronized";
    JsonObject player = payload["player"].to<JsonObject>();
    player["volume"] = static_cast<int>(volume * 100.0f);
    player["muted"] = muted;
  });
}

// Outbound: controller commands (play/pause, next, previous, set_volume)
inline std::string build_controller_command(const std::string &command) {
  return json::build_json([&](JsonObject root) {
    root["type"] = "server/command";  // client sends this to request a command
    root["payload"]["command"] = command;
  });
}

inline std::string build_set_volume_command(float volume) {
  return json::build_json([&](JsonObject root) {
    root["type"] = "server/command";
    JsonObject payload = root["payload"].to<JsonObject>();
    payload["command"] = "volume";
    payload["volume"] = static_cast<int>(volume * 100.0f);
  });
}

// Outbound: client/goodbye
inline std::string build_client_goodbye() {
  return json::build_json([](JsonObject root) {
    root["type"] = "client/goodbye";
    root["payload"].to<JsonObject>();  // MA requires a payload object
  });
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
    if (!root["payload"].is<JsonObject>())
      return false;
    JsonObject payload = root["payload"].as<JsonObject>();
    if (!payload["player"].is<JsonObject>())
      return false;
    JsonObject player = payload["player"].as<JsonObject>();
    if (!player["codec"].is<std::string>())
      return false;
    out.codec = player["codec"].as<std::string>();
    if (player["sample_rate"].is<int>())
      out.sample_rate = player["sample_rate"].as<int>();
    if (player["channels"].is<int>())
      out.channels = player["channels"].as<int>();
    if (player["bit_depth"].is<int>())
      out.bit_depth = player["bit_depth"].as<int>();
    if (player["start_ts_us"].is<int64_t>())
      out.start_ts_us = player["start_ts_us"].as<int64_t>();
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
    if (!root["payload"].is<JsonObject>())
      return false;
    JsonObject payload = root["payload"].as<JsonObject>();
    if (!payload["client_transmitted"].is<int64_t>() || !payload["server_received"].is<int64_t>() ||
        !payload["server_transmitted"].is<int64_t>())
      return false;
    out.client_transmitted = payload["client_transmitted"].as<int64_t>();
    out.server_received = payload["server_received"].as<int64_t>();
    out.server_transmitted = payload["server_transmitted"].as<int64_t>();
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
