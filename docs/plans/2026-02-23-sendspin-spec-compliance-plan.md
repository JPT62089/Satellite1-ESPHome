# Sendspin Spec Compliance Fix — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix five spec deviations that prevent Music Assistant from discovering and streaming to the device via Sendspin.

**Architecture:** Three files touched, no schema or YAML changes. The transport layer (device hosts WebSocket server, MA connects) is correct — only the mDNS advertisement and application-layer message formats need fixing.

**Tech Stack:** ESP-IDF mDNS (`mdns_txt_item_t`), ArduinoJson (used via ESPHome's `json::build_json` / `json::parse_json`), ESPHome component lifecycle.

**Design doc:** `docs/plans/2026-02-23-sendspin-spec-compliance-design.md`

---

### Task 1: Add mDNS TXT records

The `_sendspin._tcp` mDNS advertisement currently has no TXT records. The spec requires `path=/sendspin`; without it MA silently ignores the service. Also add `name` so MA can label the device before connecting.

**Files:**
- Modify: `esphome/components/sendspin/sendspin_stream.cpp:21`

**Step 1: Make the change**

Replace line 21 in `sendspin_stream.cpp`:

```cpp
  // Register mDNS service so MA can discover this device
  esp_err_t err = mdns_service_add(nullptr, "_sendspin", "_tcp", this->port_, nullptr, 0);
```

With:

```cpp
  // Register mDNS service so MA can discover this device.
  // path TXT record is required by spec; name is optional but useful.
  mdns_txt_item_t txt[] = {{"path", "/sendspin"}, {"name", App.get_friendly_name().c_str()}};
  esp_err_t err = mdns_service_add(nullptr, "_sendspin", "_tcp", this->port_, txt, 2);
```

`App.get_friendly_name()` is already used elsewhere in the file's translation unit (via `sendspin_client.cpp`), but `sendspin_stream.cpp` includes `esphome/core/application.h` so `App` is available directly.

**Step 2: Verify it compiles**

```bash
source .venv/bin/activate
esphome compile config/satellite1.yaml 2>&1 | tail -5
```

Expected: `INFO Successfully compiled program.`

**Step 3: Commit**

```bash
git add esphome/components/sendspin/sendspin_stream.cpp
git commit -m "fix(sendspin): add path and name TXT records to mDNS advertisement"
```

---

### Task 2: Fix message builders — `data` → `payload` and `client/hello` shape

All outbound JSON messages use `"data"` as the payload key; the spec requires `"payload"`. Also `client/hello` has the wrong shape for `supported_roles` (should be an array of strings with capabilities as flat sibling keys, not an array of role objects).

**Files:**
- Modify: `esphome/components/sendspin/messages.h:32-97`

**Step 1: Replace `build_client_hello` (lines 32–55)**

```cpp
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
  });
}
```

**Step 2: Replace `build_client_time` (lines 57–63)**

```cpp
// Outbound: client/time — clock sync request, ts_us is esp_timer_get_time() value
inline std::string build_client_time(int64_t ts_us) {
  return json::build_json([&](JsonObject root) {
    root["type"] = "client/time";
    root["payload"]["client_transmitted"] = ts_us;
  });
}
```

**Step 3: Replace `build_client_state` (lines 65–75)**

```cpp
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
```

**Step 4: Replace `build_controller_command` and `build_set_volume_command` (lines 77–92)**

```cpp
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
```

**Step 5: Verify it compiles**

```bash
esphome compile config/satellite1.yaml 2>&1 | tail -5
```

Expected: `INFO Successfully compiled program.`

**Step 6: Commit**

```bash
git add esphome/components/sendspin/messages.h
git commit -m "fix(sendspin): fix client/hello shape and rename data->payload in all outbound messages"
```

---

### Task 3: Fix inbound message parsers — `data` → `payload`, `stream/start` nesting

Two inbound parsers read from `root["data"]` but the spec uses `root["payload"]`. Additionally, `parse_stream_start` looks for `codec` directly on the payload object, but the spec nests it under `payload["player"]`.

**Files:**
- Modify: `esphome/components/sendspin/messages.h:138-154` (`parse_server_time`)
- Modify: `esphome/components/sendspin/messages.h:108-129` (`parse_stream_start`)

**Step 1: Replace `parse_stream_start` (lines 108–129)**

```cpp
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
```

**Step 2: Replace `parse_server_time` (lines 138–154)**

```cpp
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
```

**Step 3: Verify it compiles**

```bash
esphome compile config/satellite1.yaml 2>&1 | tail -5
```

Expected: `INFO Successfully compiled program.`

**Step 4: Commit**

```bash
git add esphome/components/sendspin/messages.h
git commit -m "fix(sendspin): fix inbound parsers to use payload key and stream/start player nesting"
```

---

### Task 4: Send initial `client/state` after `server/hello`

The spec requires `client/state` immediately after receiving `server/hello` (when the state machine enters READY). Currently it is only sent when `report_volume()` is called externally. To always have a valid value to send, store the last-known volume/muted in `SendspinClient` and update it in `report_volume()`.

**Files:**
- Modify: `esphome/components/sendspin/sendspin_client.h:43` (add two members)
- Modify: `esphome/components/sendspin/sendspin_client.cpp:59-63` (`report_volume`)
- Modify: `esphome/components/sendspin/sendspin_client.cpp:80-81` (`on_stream_state_changed_`, READY branch)

**Step 1: Add members to `SendspinClient` in `sendspin_client.h`**

After the existing `bool clock_synced_{false};` line (around line 54), add:

```cpp
  float last_volume_{1.0f};
  bool last_muted_{false};
```

**Step 2: Update `report_volume` in `sendspin_client.cpp` to cache the values**

Replace the existing `report_volume` implementation (lines 59–63):

```cpp
void SendspinClient::report_volume(float volume, bool muted) {
  this->last_volume_ = volume;
  this->last_muted_ = muted;
  if (this->stream_.is_connected()) {
    this->stream_.send_text(build_client_state(volume, muted));
  }
}
```

**Step 3: Send initial `client/state` in the READY branch of `on_stream_state_changed_`**

The READY branch (around line 80–81) currently reads:

```cpp
  } else if (state == SendspinStreamState::READY) {
    this->clock_synced_ = false;
```

Replace it with:

```cpp
  } else if (state == SendspinStreamState::READY) {
    this->clock_synced_ = false;
    this->stream_.send_text(build_client_state(this->last_volume_, this->last_muted_));
```

**Step 4: Verify it compiles**

```bash
esphome compile config/satellite1.yaml 2>&1 | tail -5
```

Expected: `INFO Successfully compiled program.`

**Step 5: Commit**

```bash
git add esphome/components/sendspin/sendspin_client.h esphome/components/sendspin/sendspin_client.cpp
git commit -m "fix(sendspin): send initial client/state immediately after server/hello"
```

---

### Task 5: Compile test and push

Verify the full firmware compiles cleanly, then push.

**Step 1: Full compile**

```bash
esphome compile config/satellite1.yaml 2>&1 | tail -5
esphome compile config/satellite1.ld2410.yaml 2>&1 | tail -5
esphome compile config/satellite1.ld2450.yaml 2>&1 | tail -5
```

Expected: all three end with `INFO Successfully compiled program.`

**Step 2: Push**

```bash
git push
```

**Step 3: Manual verification**

Flash the device and confirm:
1. Device logs show `mdns_service_add` does not log a warning (service registered successfully)
2. MA shows the device as an available Sendspin player
3. Selecting the device in MA causes a WebSocket connection and `[I][sendspin_stream:...]: MA connected` appears in device logs
4. Playback starts and audio is heard
