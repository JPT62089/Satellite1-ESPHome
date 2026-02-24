# Sendspin Spec Compliance Fix — Design

**Date:** 2026-02-23

## Overview

The Sendspin component has three categories of spec deviations that prevent Music Assistant from
discovering and connecting to the device:

1. Missing mDNS TXT records — MA finds the service but doesn't know the WebSocket path
2. All messages use `"data"` key — spec requires `"payload"`
3. `client/hello` role structure is wrong, and `stream/start` parser looks in the wrong place
4. Initial `client/state` is never sent after `server/hello`

The architecture (server-initiated, device hosts WebSocket server) is correct per spec. No
transport-layer rewrite is needed.

## Root Cause

`mdns_service_add` is called with `nullptr, 0` for TXT items. The Sendspin spec requires a `path`
TXT record so MA knows which WebSocket endpoint to connect to. Without it, MA silently ignores the
advertised service.

## Changes

### `sendspin_stream.cpp` — mDNS TXT records

Add `path` and `name` TXT records to the `_sendspin._tcp` advertisement:

```cpp
mdns_txt_item_t txt[] = {{"path", "/sendspin"}, {"name", App.get_friendly_name().c_str()}};
mdns_service_add(nullptr, "_sendspin", "_tcp", this->port_, txt, 2);
```

### `messages.h` — Protocol message compliance

**`build_client_hello`** — fix key name, flatten role capabilities:

```json
{
  "type": "client/hello",
  "payload": {
    "client_id": "<mac>",
    "name": "<friendly_name>",
    "version": 1,
    "supported_roles": ["player@v1", "controller@v1"],
    "player@v1_support": {
      "supported_formats": [{"codec":"flac","channels":2,"sample_rate":48000,"bit_depth":16}],
      "buffer_capacity": 131072
    }
  }
}
```

Previously `supported_roles` was an array of `{"role": "...", "player_support": {...}}` objects.
Spec requires an array of role-name strings with capabilities as flat sibling keys.

**`build_client_time`** — `data` → `payload`

**`build_client_state`** — `data` → `payload`

**`build_controller_command` / `build_set_volume_command`** — `data` → `payload`

**`parse_server_time`** — `root["data"]` → `root["payload"]`

**`parse_stream_start`** — fix two things:
- `root["data"]` → `root["payload"]`
- All stream fields (`codec`, `sample_rate`, `channels`, `bit_depth`) are nested under
  `payload["player"]`, not directly on `payload`

### `sendspin_client.cpp` — Initial `client/state` on READY

The spec requires `client/state` to be sent immediately after receiving `server/hello` (READY
state). Currently it is only sent when `report_volume()` is called. Add an initial send in
`on_stream_state_changed_()`:

```cpp
} else if (state == SendspinStreamState::READY) {
    this->clock_synced_ = false;
    if (this->media_player_ != nullptr)
        this->stream_.send_text(build_client_state(
            this->media_player_->volume, this->media_player_->is_muted()));
```

## Files Touched

| File | Change |
|------|--------|
| `esphome/components/sendspin/sendspin_stream.cpp` | Add `path` + `name` TXT records to `mdns_service_add` |
| `esphome/components/sendspin/messages.h` | `data`→`payload` in all builders/parsers; fix `client/hello` role shape; fix `parse_stream_start` nesting |
| `esphome/components/sendspin/sendspin_client.cpp` | Send initial `client/state` when entering READY state |
