# Sendspin Integration Design

**Date:** 2026-02-21
**Status:** Approved
**Scope:** Player + Controller roles, FLAC only

## Background

Sendspin is the Open Home Foundation's multi-room audio protocol and Music Assistant's native playback mechanism. It uses WebSocket (JSON control + binary audio) with mDNS discovery. The Satellite1 already has Snapcast for multi-room audio; Sendspin coexists alongside it as a second music streaming path, enabled independently via a switch.

## Architecture

A new custom ESPHome component in `esphome/components/sendspin/` structured similarly to the existing `snapcast` component. Two functional halves:

- **Player** — WebSocket client connecting to MA, receives binary FLAC-encoded audio chunks, decodes via the existing `audio::AudioDecoder`, feeds into the mixer pipeline at `media_mixing_input`.
- **Controller** — sends JSON playback commands (play/pause, next, previous, volume) back to MA over the same WebSocket connection, exposed as ESPHome actions wired to button presses via YAML automations.

Sendspin and Snapcast coexist; no hard interlock. Playing both simultaneously mixes at `media_mixing_input` — discouraged in docs but not enforced in code.

## Component Structure

```
esphome/components/sendspin/
  __init__.py                  # ESPHome config schema and validation
  automation.h                 # play_pause, next, previous, set_volume actions
  sendspin_client.h / .cpp     # WebSocket lifecycle, role negotiation, clock sync
  sendspin_stream.h / .cpp     # binary audio frame handling, FLAC decode → mixer
  sendspin_controller.h / .cpp # outbound JSON controller commands
  messages.h / .cpp            # inbound/outbound JSON message types
```

`SensdpinClient` is the top-level ESPHome component, owns the WebSocket connection, dispatches binary frames to `SensdpinStream` and JSON frames to message handlers. `SensdpinController` holds the outbound command queue. `SensdpinStream` reuses `audio::AudioDecoder` and writes decoded PCM to the mixer speaker.

## Connection & Protocol Flow

1. **mDNS advertisement** — On startup, register `_sendspin._tcp.local` via ESPHome's mDNS. MA uses this to discover the device and initiates the WebSocket connection (server-initiated model per spec).
2. **WebSocket** — Uses ESP-IDF's `esp_websocket_client`. Configurable port, default `7777`, advertised in the mDNS TXT record.
3. **Role negotiation** — On connect, send JSON hello declaring roles `["player@v1", "controller@v1"]` and device name. MA responds with stream assignment and group membership.
4. **Clock sync** — Implement the Sendspin clock sync handshake (round-trip timestamp exchange) using `esp_timer_get_time()`. Produces a local→server offset for scheduling audio at the correct wall-clock time.
5. **Steady state** — Binary frames carry FLAC-encoded audio with a target playback timestamp. JSON frames carry group state changes.
6. **Reconnection** — On disconnect, exponential backoff (1s → 2s → 4s → max 30s), re-advertise mDNS. MA reconnects automatically.

## Audio Pipeline Integration

Sendspin feeds into the existing mixer pipeline unchanged:

```
SensdpinStream (FLAC decode)
  → media_resampling_speaker   (resampler, 48kHz/16-bit)
    → media_mixing_input       (mixer input, 100ms buffer)
      → mixing_speaker         (mixer)
        → i2s_audio_speaker    (I2S → XMOS → DAC)
```

The component advertises `flac` as its supported codec during role setup. No changes to the resampler, mixer, or I2S speaker components.

YAML config in `media_player.yaml`:

```yaml
sendspin:
  id: sendspin_client

switch:
  - platform: template
    id: sendspin_switch
    name: Sendspin
    icon: "mdi:music-circle"
    entity_category: config
    optimistic: true
    restore_mode: RESTORE_DEFAULT_ON
    on_turn_on:
      - sendspin.enable:
    on_turn_off:
      - sendspin.disable:
```

## Controller Integration

The component exposes ESPHome actions:

```
sendspin.play_pause
sendspin.next
sendspin.previous
sendspin.set_volume (value: float)
```

- **Single press (action button)** — `handle_single_press` script checks if Sendspin is active and music is playing; if so, calls `sendspin.play_pause` instead of starting the voice assistant.
- **Volume up/down** — already routed through `media_player.volume_up/down` → HA API → MA. No change needed.
- **Next/previous** — double/triple press currently fires HA events. User wires those events to `sendspin.next` / `sendspin.previous` in HA automations, or calls the actions directly from `buttons.yaml`. The component does not hard-code button behavior.

## Error Handling

| Condition | Behavior |
|-----------|----------|
| MA not found | Stay idle, re-advertise mDNS every 30s |
| WebSocket disconnect | Drain buffer, stop stream, reconnect with backoff |
| FLAC decode error | Drop frame, log WARN, continue |
| Clock sync failure | Proceed without offset after 3 attempts, log WARN |
| `sendspin.disable` while playing | Send disconnect per spec, close WebSocket, silence mixer input |
| Late / out-of-order frame | Drop frame, log WARN |

## Testing

- **Compile test** — `tests/components/sendspin/test_sendspin.yaml` validates component compiles with player + controller configured.
- **Integration** — Flash to Satellite1, confirm MA discovers via mDNS and streams FLAC audio to speaker.
- **Controller** — Trigger `sendspin.play_pause` via HA developer tools, confirm MA pauses.
- **Reconnect** — Restart MA mid-playback, confirm device reconnects within backoff window.
- **Coexistence** — Enable both Snapcast and Sendspin switches simultaneously, confirm device remains stable.
- **Clock sync** — Add device to a Sendspin group with another player, ear-test synchronization.

CI gate: clang-format + yamllint + ESPHome compile test (same as existing components).

## Out of Scope

- Opus codec support (MA uses FLAC for local connections; Opus can be added later)
- Metadata role (HA already receives track info from MA independently)
- Artwork and Visualizer roles
