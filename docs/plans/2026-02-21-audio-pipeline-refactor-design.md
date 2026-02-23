# Audio Pipeline Refactor Design

**Date:** 2026-02-21
**Status:** Approved
**Scope:** `esphome/components/speaker/media_player/audio_pipeline.{h,cpp}` and `speaker_media_player.{h,cpp}`

## Problem

Two related pain points:

1. **Extensibility** — each new audio source (URL, file, Snapcast, future Sendspin) adds a `pending_*` boolean, a source data member, a `READER_COMMAND_INIT_*` event bit, and a `#if USE_*` conditional block in at least six places across two files. The pattern doesn't scale.

2. **Complexity** — `process_state()` is ~140 lines doing four distinct jobs. `watch_media_commands_()` is ~160 lines mixing playlist management, transport dispatch, and pipeline routing.

## Approach

Approach A: `PipelineSource` struct + helper method decomposition. No new abstractions, no vtables — same logic reorganised.

---

## Section 1: `PipelineSource` — consolidate source storage

### Before

```cpp
// 3 booleans + 3 members + 3 event bits + #if USE_SNAPCAST in 6 places
bool pending_url_;
bool pending_file_;
bool pending_snapcast_;          // #if USE_SNAPCAST
std::string current_uri_;
audio::AudioFile *current_audio_file_;
snapcast::SnapcastStream *snapcast_stream_;  // #if USE_SNAPCAST

READER_COMMAND_INIT_HTTP    = (1 << 4),
READER_COMMAND_INIT_FILE    = (1 << 5),
READER_COMMAND_INIT_SNAPCAST = (1 << 6),  // #if USE_SNAPCAST
```

### After

```cpp
enum class PipelineSourceType : uint8_t {
  URL,
  FILE,
#if USE_SNAPCAST
  SNAPCAST,
#endif
};

struct PipelineSource {
  PipelineSourceType type;
  std::string uri;
  audio::AudioFile *audio_file{nullptr};
#if USE_SNAPCAST
  snapcast::SnapcastStream *snapcast_stream{nullptr};
#endif
};

// In AudioPipeline:
optional<PipelineSource> pending_source_;
optional<PipelineSource> current_source_;

// Single event bit replaces three:
READER_COMMAND_INIT = (1 << 4),
```

`read_task` dispatches on `current_source_->type` via a `switch`. Adding Sendspin requires one new enum value, one `#if USE_SENDSPIN` block in the struct, and one `case` in the switch — no other files touched.

---

## Section 2: Decompose `process_state()`

### New private helpers on `AudioPipeline`

| Method | Responsibility |
|--------|---------------|
| `drain_info_queue_()` | Drain and log all `InfoErrorEvent` items from the queue |
| `try_start_pending_()` | If `pending_source_` is set and no stop is pending, start tasks, copy to `current_source_`, signal `READER_COMMAND_INIT`; return true |
| `check_errors_()` | Check `READER/DECODER_MESSAGE_ERROR` bits; clear them; return the error state if found |
| `check_completion_()` | Detect reader + decoder finished with no media in-flight; drive `speaker->finish()` or `speaker->stop()`; call `delete_tasks_()`; return true while winding down |

### Resulting `process_state()`

```cpp
AudioPipelineState AudioPipeline::process_state() {
  this->drain_info_queue_();

  if (this->try_start_pending_())
    return AudioPipelineState::PLAYING;

  AudioPipelineState err = this->check_errors_();
  if (err != AudioPipelineState::PLAYING)
    return err;

  if (this->check_completion_())
    return AudioPipelineState::STOPPING;

  if (this->read_task_handle_ == nullptr && this->decode_task_handle_ == nullptr) {
    xEventGroupClearBits(this->event_group_, PIPELINE_COMMAND_STOP);
    this->is_playing_ = false;
    return AudioPipelineState::STOPPED;
  }

  if (xEventGroupGetBits(this->event_group_) & PIPELINE_COMMAND_STOP)
    return AudioPipelineState::STOPPING;

  if (this->pause_state_)
    return AudioPipelineState::PAUSED;

  this->is_playing_ = true;
  return AudioPipelineState::PLAYING;
}
```

Each helper is ~20–30 lines, independently readable.

---

## Section 3: Decompose `watch_media_commands_()`

### New private helpers on `SpeakerMediaPlayer`

| Method | Responsibility |
|--------|---------------|
| `handle_play_item_(cmd)` | Route URL/file to announcement or media playlist; stop current pipeline if not enqueuing; handle the paused-pipeline retry edge case |
| `handle_transport_command_(cmd)` | 8-case switch: PLAY, PAUSE, STOP, TOGGLE, MUTE, UNMUTE, VOLUME_UP/DOWN, REPEAT_ONE/OFF, CLEAR_PLAYLIST |

### Resulting `watch_media_commands_()`

```cpp
void SpeakerMediaPlayer::watch_media_commands_() {
  if (!this->is_ready()) return;
  MediaCallCommand cmd;
  if (xQueueReceive(this->media_control_command_queue_, &cmd, 0) != pdTRUE) return;

  if (cmd.url.has_value() || cmd.file.has_value()) {
    this->handle_play_item_(cmd);
    return;
  }
  if (cmd.volume.has_value()) {
    this->set_volume_(cmd.volume.value());
    this->publish_state();
    return;
  }
  if (cmd.command.has_value()) {
    this->handle_transport_command_(cmd);
  }
}
```

No logic changes — identical branching and edge cases, just decomposed.

---

## Files Changed

- `esphome/components/speaker/media_player/audio_pipeline.h` — add `PipelineSource` struct/enum, replace source members, update event bits, declare new private helpers
- `esphome/components/speaker/media_player/audio_pipeline.cpp` — implement helpers, update `process_state()`, update `read_task` switch, update `start_url/start_file/start_snapcast`
- `esphome/components/speaker/media_player/speaker_media_player.h` — declare `handle_play_item_`, `handle_transport_command_`
- `esphome/components/speaker/media_player/speaker_media_player.cpp` — implement helpers, simplify `watch_media_commands_()`

## Out of Scope

- Adding Sendspin support (separate task, this refactor makes it easy)
- Changing task stack sizes, buffer sizes, or FreeRTOS priorities
- Modifying the decoder task or the speaker interface
