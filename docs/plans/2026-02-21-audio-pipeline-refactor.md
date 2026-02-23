# Audio Pipeline Refactor Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Consolidate per-source booleans/members into a `PipelineSource` struct and decompose `process_state()` and `watch_media_commands_()` into focused helper methods.

**Architecture:** Replace three `pending_*` booleans and three source data members with `optional<PipelineSource>`. Collapse three `READER_COMMAND_INIT_*` event bits to one. Extract four helpers from `process_state()` and two from `watch_media_commands_()`. No logic changes — identical behaviour, just reorganised.

**Tech Stack:** C++17, ESP-IDF FreeRTOS, ESPHome component API, clang-format v18

**Design doc:** `docs/plans/2026-02-21-audio-pipeline-refactor-design.md`

---

### Task 1: Setup — worktree and baseline compile

**Files:** none (setup only)

**Step 1: Create a worktree**

```bash
git worktree add .claude/worktrees/audio-pipeline-refactor -b feature/audio-pipeline-refactor
cd .claude/worktrees/audio-pipeline-refactor
```

**Step 2: Activate the virtual environment**

```bash
source scripts/setup_build_env.sh
# or if already set up:
source .venv/bin/activate
```

**Step 3: Establish baseline — compile all three configs**

```bash
esphome compile config/satellite1.yaml
esphome compile config/satellite1.ld2410.yaml
esphome compile config/satellite1.ld2450.yaml
```

Expected: all three compile cleanly with no errors. If any fail, stop and investigate before proceeding.

---

### Task 2: PipelineSource type + source consolidation

**Files:**
- Modify: `esphome/components/speaker/media_player/audio_pipeline.h`
- Modify: `esphome/components/speaker/media_player/audio_pipeline.cpp`

#### Step 1: Update the header

Open `audio_pipeline.h`. Make these changes in order:

**1a. Add `PipelineSourceType` enum and `PipelineSource` struct** after the `AudioPipelineType` enum (line ~31):

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
```

**1b. Add four private helper declarations** at the bottom of the `protected:` section (after `delete_tasks_()` declaration):

```cpp
  void drain_info_queue_();
  bool try_start_pending_();
  AudioPipelineState check_errors_();
  bool check_completion_();
```

**1c. Remove the three old source booleans** from the `protected:` section:

Remove these lines:
```cpp
  bool pending_url_{false};
  bool pending_file_{false};
#if USE_SNAPCAST
  bool pending_snapcast_{false};
#endif
```

**1d. Replace the three old source data members** in the `protected:` section.

Remove:
```cpp
  std::string current_uri_{};
  audio::AudioFile *current_audio_file_{nullptr};
#if USE_SNAPCAST
  snapcast::SnapcastStream *snapcast_stream_{nullptr};
#endif
```

Add in their place:
```cpp
  optional<PipelineSource> pending_source_;
  optional<PipelineSource> current_source_;
```

#### Step 2: Update `audio_pipeline.cpp` — event bits

Replace the three `READER_COMMAND_INIT_*` bits in the `EventGroupBits` enum with one. Find:

```cpp
  // Read audio from an HTTP source; cleared by reader task and set by start_url
  READER_COMMAND_INIT_HTTP = (1 << 4),
  // Read audio from an audio file from the flash; cleared by reader task and set by start_file
  READER_COMMAND_INIT_FILE = (1 << 5),
  // Read audio from a snapcast server; cleared by reader task and set by start_snapcast
  READER_COMMAND_INIT_SNAPCAST = (1 << 6),
```

Replace with:

```cpp
  // Initialise the reader with current_source_; cleared by reader task
  READER_COMMAND_INIT = (1 << 4),
```

#### Step 3: Update `start_url`, `start_file`, `start_snapcast`

Replace `start_url`:

```cpp
void AudioPipeline::start_url(const std::string &uri) {
  if (this->is_playing_) {
    xEventGroupSetBits(this->event_group_, PIPELINE_COMMAND_STOP);
  }
  PipelineSource source;
  source.type = PipelineSourceType::URL;
  source.uri = uri;
  this->pending_source_ = source;
}
```

Replace `start_file`:

```cpp
void AudioPipeline::start_file(audio::AudioFile *audio_file) {
  if (this->is_playing_) {
    xEventGroupSetBits(this->event_group_, PIPELINE_COMMAND_STOP);
  }
  PipelineSource source;
  source.type = PipelineSourceType::FILE;
  source.audio_file = audio_file;
  this->pending_source_ = source;
}
```

Replace `start_snapcast` (inside the `#if USE_SNAPCAST` guard):

```cpp
#if USE_SNAPCAST
void AudioPipeline::start_snapcast(snapcast::SnapcastStream *stream) {
  if (this->is_playing_) {
    xEventGroupSetBits(this->event_group_, PIPELINE_COMMAND_STOP);
  }
  PipelineSource source;
  source.type = PipelineSourceType::SNAPCAST;
  source.snapcast_stream = stream;
  this->pending_source_ = source;
}
#endif
```

#### Step 4: Update `process_state()` — remove old pending checks

In `process_state()`, find the outer pending check (currently lines ~165-169):

```cpp
#if USE_SNAPCAST
  if (this->pending_url_ || this->pending_file_ || this->pending_snapcast_) {
#else
  if (this->pending_url_ || this->pending_file_) {
#endif
```

Replace the whole block (including the inner dispatch for `pending_url_`, `pending_file_`, `pending_snapcast_` and the `READER_COMMAND_INIT_HTTP/FILE/SNAPCAST` calls) with the consolidated version. The full replacement for lines ~165-198 is:

```cpp
  if (this->pending_source_.has_value()) {
    // Init command pending
    EventBits_t event_bits = xEventGroupGetBits(this->event_group_);
    if (!(event_bits & EventGroupBits::PIPELINE_COMMAND_STOP) && !this->is_playing_) {
      // Only start if there is no pending stop command
      if ((this->read_task_handle_ == nullptr) || (this->decode_task_handle_ == nullptr)) {
        // At least one task isn't running
        this->start_tasks_();
      }

      this->current_source_ = this->pending_source_;
      this->pending_source_.reset();
      this->playback_ms_ = 0;
      xEventGroupSetBits(this->event_group_, EventGroupBits::READER_COMMAND_INIT);

      this->is_playing_ = true;
      this->hard_stop_ = false;
      return AudioPipelineState::PLAYING;
    }
  }
```

Note: the local `event_bits` variable that was declared earlier in `process_state()` (line ~163) is no longer needed at that point — move the `xEventGroupGetBits` call into each section that needs it, or keep the local and adjust. The cleanest approach: remove the early `EventBits_t event_bits = xEventGroupGetBits(this->event_group_);` line (line ~163) since each branch now fetches bits when it needs them. Update the subsequent checks (READER_MESSAGE_ERROR, DECODER_MESSAGE_ERROR, READER_MESSAGE_FINISHED etc.) to call `xEventGroupGetBits` directly.

Full updated `process_state()` after this task (before helper extraction):

```cpp
AudioPipelineState AudioPipeline::process_state() {
  /*
   * Log items from info error queue
   */
  InfoErrorEvent event;
  if (this->info_error_queue_ != nullptr) {
    while (xQueueReceive(this->info_error_queue_, &event, 0)) {
      switch (event.source) {
        case InfoErrorSource::READER:
          if (event.err.has_value()) {
            ESP_LOGE(TAG, "Media reader encountered an error: %s", esp_err_to_name(event.err.value()));
          } else if (event.file_type.has_value()) {
            ESP_LOGD(TAG, "Reading %s file type", audio_file_type_to_string(event.file_type.value()));
          }
          break;
        case InfoErrorSource::DECODER:
          if (event.err.has_value()) {
            ESP_LOGE(TAG, "Decoder encountered an error: %s", esp_err_to_name(event.err.value()));
          }
          if (event.audio_stream_info.has_value()) {
            ESP_LOGD(TAG, "Decoded audio has %d channels, %" PRId32 " Hz sample rate, and %d bits per sample",
                     event.audio_stream_info.value().get_channels(), event.audio_stream_info.value().get_sample_rate(),
                     event.audio_stream_info.value().get_bits_per_sample());
          }
          if (event.decoding_err.has_value()) {
            switch (event.decoding_err.value()) {
              case DecodingError::FAILED_HEADER:
                ESP_LOGE(TAG, "Failed to parse the file's header.");
                break;
              case DecodingError::INCOMPATIBLE_BITS_PER_SAMPLE:
                ESP_LOGE(TAG, "Incompatible bits per sample. Only 16 bits per sample is supported");
                break;
              case DecodingError::INCOMPATIBLE_CHANNELS:
                ESP_LOGE(TAG, "Incompatible number of channels. Only 1 or 2 channel audio is supported.");
                break;
            }
          }
          break;
      }
    }
  }

  if (this->pending_source_.has_value()) {
    EventBits_t event_bits = xEventGroupGetBits(this->event_group_);
    if (!(event_bits & EventGroupBits::PIPELINE_COMMAND_STOP) && !this->is_playing_) {
      if ((this->read_task_handle_ == nullptr) || (this->decode_task_handle_ == nullptr)) {
        this->start_tasks_();
      }
      this->current_source_ = this->pending_source_;
      this->pending_source_.reset();
      this->playback_ms_ = 0;
      xEventGroupSetBits(this->event_group_, EventGroupBits::READER_COMMAND_INIT);
      this->is_playing_ = true;
      this->hard_stop_ = false;
      return AudioPipelineState::PLAYING;
    }
  }

  EventBits_t event_bits = xEventGroupGetBits(this->event_group_);

  if ((event_bits & EventGroupBits::READER_MESSAGE_ERROR)) {
    xEventGroupClearBits(this->event_group_, EventGroupBits::READER_MESSAGE_ERROR);
    return AudioPipelineState::ERROR_READING;
  }

  if ((event_bits & EventGroupBits::DECODER_MESSAGE_ERROR)) {
    xEventGroupClearBits(this->event_group_, EventGroupBits::DECODER_MESSAGE_ERROR);
    return AudioPipelineState::ERROR_DECODING;
  }

  if ((this->read_task_handle_ == nullptr) && (this->decode_task_handle_ == nullptr)) {
    xEventGroupClearBits(this->event_group_, EventGroupBits::PIPELINE_COMMAND_STOP);
    this->is_playing_ = false;
    return AudioPipelineState::STOPPED;
  }

  if ((event_bits & EventGroupBits::READER_MESSAGE_FINISHED) &&
      (!(event_bits & EventGroupBits::READER_MESSAGE_LOADED_MEDIA_TYPE) &&
       (event_bits & EventGroupBits::DECODER_MESSAGE_FINISHED))) {
    if (event_bits & EventGroupBits::PIPELINE_COMMAND_STOP) {
      this->hard_stop_ = true;
    }
    if ((this->read_task_handle_ != nullptr) || (this->decode_task_handle_ != nullptr)) {
      if (this->speaker_ != nullptr && !this->speaker_->is_stopped()) {
        if (this->hard_stop_) {
          this->speaker_->stop();
          this->hard_stop_ = false;
        } else {
          this->speaker_->finish();
        }
      } else {
        this->delete_tasks_();
      }
    }
    return AudioPipelineState::STOPPING;
  }

  if (event_bits & EventGroupBits::PIPELINE_COMMAND_STOP) {
    return AudioPipelineState::STOPPING;
  }

  if (this->pause_state_) {
    return AudioPipelineState::PAUSED;
  }

  this->is_playing_ = true;
  return AudioPipelineState::PLAYING;
}
```

#### Step 5: Update `read_task` — source dispatch

In `read_task`, find the `waiting_bits` declaration and the `xEventGroupClearBits` call:

Replace the `waiting_bits` declaration:
```cpp
    // OLD
    const EventBits_t waiting_bits =
        (EventGroupBits::READER_COMMAND_INIT_FILE | EventGroupBits::READER_COMMAND_INIT_HTTP
#if USE_SNAPCAST
         | EventGroupBits::READER_COMMAND_INIT_SNAPCAST
#endif
         | EventGroupBits::PIPELINE_COMMAND_STOP);
```

With:
```cpp
    const EventBits_t waiting_bits =
        (EventGroupBits::READER_COMMAND_INIT | EventGroupBits::PIPELINE_COMMAND_STOP);
```

Replace the `xEventGroupClearBits` call inside the `if (!(event_bits & PIPELINE_COMMAND_STOP))` block:
```cpp
      // OLD
      xEventGroupClearBits(this_pipeline->event_group_, EventGroupBits::READER_MESSAGE_FINISHED |
                                                            EventGroupBits::READER_COMMAND_INIT_FILE |
                                                            EventGroupBits::READER_COMMAND_INIT_HTTP
#if USE_SNAPCAST
                                                            | EventGroupBits::READER_COMMAND_INIT_SNAPCAST
#endif
      );
```

With:
```cpp
      xEventGroupClearBits(this_pipeline->event_group_,
                           EventGroupBits::READER_MESSAGE_FINISHED | EventGroupBits::READER_COMMAND_INIT);
```

Replace the per-source `if/else if` reader start dispatch:
```cpp
      // OLD
      if (event_bits & EventGroupBits::READER_COMMAND_INIT_FILE) {
        err = reader->start(this_pipeline->current_audio_file_, this_pipeline->current_audio_file_type_);
      } else if (event_bits & EventGroupBits::READER_COMMAND_INIT_HTTP) {
        err = reader->start(this_pipeline->current_uri_, this_pipeline->current_audio_file_type_);
      }
#if USE_SNAPCAST
      else if (event_bits & EventGroupBits::READER_COMMAND_INIT_SNAPCAST) {
        err = reader->start(this_pipeline->snapcast_stream_, this_pipeline->current_audio_file_type_);
      }
#endif
```

With:
```cpp
      const PipelineSource &src = this_pipeline->current_source_.value();
      switch (src.type) {
        case PipelineSourceType::FILE:
          err = reader->start(src.audio_file, this_pipeline->current_audio_file_type_);
          break;
        case PipelineSourceType::URL:
          err = reader->start(src.uri, this_pipeline->current_audio_file_type_);
          break;
#if USE_SNAPCAST
        case PipelineSourceType::SNAPCAST:
          err = reader->start(src.snapcast_stream, this_pipeline->current_audio_file_type_);
          break;
#endif
      }
```

#### Step 6: Compile

```bash
esphome compile config/satellite1.yaml
```

Expected: compiles cleanly. If errors, fix before proceeding.

#### Step 7: Run clang-format

```bash
clang-format -i esphome/components/speaker/media_player/audio_pipeline.h
clang-format -i esphome/components/speaker/media_player/audio_pipeline.cpp
```

Re-compile to verify format didn't introduce issues.

#### Step 8: Compile all three configs

```bash
esphome compile config/satellite1.ld2410.yaml
esphome compile config/satellite1.ld2450.yaml
```

#### Step 9: Commit

```bash
git add esphome/components/speaker/media_player/audio_pipeline.h \
        esphome/components/speaker/media_player/audio_pipeline.cpp
git commit -m "refactor(audio_pipeline): consolidate source storage into PipelineSource struct"
```

---

### Task 3: Decompose `process_state()` into helpers

**Files:**
- Modify: `esphome/components/speaker/media_player/audio_pipeline.h` (declarations already added in Task 2)
- Modify: `esphome/components/speaker/media_player/audio_pipeline.cpp`

#### Step 1: Implement `drain_info_queue_()`

Add this new method to `audio_pipeline.cpp` (before `process_state()`):

```cpp
void AudioPipeline::drain_info_queue_() {
  if (this->info_error_queue_ == nullptr)
    return;
  InfoErrorEvent event;
  while (xQueueReceive(this->info_error_queue_, &event, 0)) {
    switch (event.source) {
      case InfoErrorSource::READER:
        if (event.err.has_value()) {
          ESP_LOGE(TAG, "Media reader encountered an error: %s", esp_err_to_name(event.err.value()));
        } else if (event.file_type.has_value()) {
          ESP_LOGD(TAG, "Reading %s file type", audio_file_type_to_string(event.file_type.value()));
        }
        break;
      case InfoErrorSource::DECODER:
        if (event.err.has_value()) {
          ESP_LOGE(TAG, "Decoder encountered an error: %s", esp_err_to_name(event.err.value()));
        }
        if (event.audio_stream_info.has_value()) {
          ESP_LOGD(TAG, "Decoded audio has %d channels, %" PRId32 " Hz sample rate, and %d bits per sample",
                   event.audio_stream_info.value().get_channels(),
                   event.audio_stream_info.value().get_sample_rate(),
                   event.audio_stream_info.value().get_bits_per_sample());
        }
        if (event.decoding_err.has_value()) {
          switch (event.decoding_err.value()) {
            case DecodingError::FAILED_HEADER:
              ESP_LOGE(TAG, "Failed to parse the file's header.");
              break;
            case DecodingError::INCOMPATIBLE_BITS_PER_SAMPLE:
              ESP_LOGE(TAG, "Incompatible bits per sample. Only 16 bits per sample is supported");
              break;
            case DecodingError::INCOMPATIBLE_CHANNELS:
              ESP_LOGE(TAG, "Incompatible number of channels. Only 1 or 2 channel audio is supported.");
              break;
          }
        }
        break;
    }
  }
}
```

#### Step 2: Implement `try_start_pending_()`

```cpp
bool AudioPipeline::try_start_pending_() {
  if (!this->pending_source_.has_value())
    return false;

  EventBits_t event_bits = xEventGroupGetBits(this->event_group_);
  if ((event_bits & EventGroupBits::PIPELINE_COMMAND_STOP) || this->is_playing_)
    return false;

  if ((this->read_task_handle_ == nullptr) || (this->decode_task_handle_ == nullptr)) {
    this->start_tasks_();
  }

  this->current_source_ = this->pending_source_;
  this->pending_source_.reset();
  this->playback_ms_ = 0;
  xEventGroupSetBits(this->event_group_, EventGroupBits::READER_COMMAND_INIT);
  this->is_playing_ = true;
  this->hard_stop_ = false;
  return true;
}
```

#### Step 3: Implement `check_errors_()`

Returns `AudioPipelineState::PLAYING` as a "no error" sentinel. Returns the error state if one is found.

```cpp
AudioPipelineState AudioPipeline::check_errors_() {
  EventBits_t event_bits = xEventGroupGetBits(this->event_group_);
  if (event_bits & EventGroupBits::READER_MESSAGE_ERROR) {
    xEventGroupClearBits(this->event_group_, EventGroupBits::READER_MESSAGE_ERROR);
    return AudioPipelineState::ERROR_READING;
  }
  if (event_bits & EventGroupBits::DECODER_MESSAGE_ERROR) {
    xEventGroupClearBits(this->event_group_, EventGroupBits::DECODER_MESSAGE_ERROR);
    return AudioPipelineState::ERROR_DECODING;
  }
  return AudioPipelineState::PLAYING;
}
```

#### Step 4: Implement `check_completion_()`

Returns `true` while the pipeline is in the process of stopping (speaker draining or tasks being deleted).

```cpp
bool AudioPipeline::check_completion_() {
  EventBits_t event_bits = xEventGroupGetBits(this->event_group_);

  if (!((event_bits & EventGroupBits::READER_MESSAGE_FINISHED) &&
        !(event_bits & EventGroupBits::READER_MESSAGE_LOADED_MEDIA_TYPE) &&
        (event_bits & EventGroupBits::DECODER_MESSAGE_FINISHED))) {
    return false;
  }

  if (event_bits & EventGroupBits::PIPELINE_COMMAND_STOP) {
    this->hard_stop_ = true;
  }

  if ((this->read_task_handle_ != nullptr) || (this->decode_task_handle_ != nullptr)) {
    if (this->speaker_ != nullptr && !this->speaker_->is_stopped()) {
      if (this->hard_stop_) {
        this->speaker_->stop();
        this->hard_stop_ = false;
      } else {
        this->speaker_->finish();
      }
    } else {
      this->delete_tasks_();
    }
  }
  return true;
}
```

#### Step 5: Replace `process_state()` body with the decomposed version

```cpp
AudioPipelineState AudioPipeline::process_state() {
  this->drain_info_queue_();

  if (this->try_start_pending_())
    return AudioPipelineState::PLAYING;

  AudioPipelineState err_state = this->check_errors_();
  if (err_state != AudioPipelineState::PLAYING)
    return err_state;

  if (this->check_completion_())
    return AudioPipelineState::STOPPING;

  EventBits_t event_bits = xEventGroupGetBits(this->event_group_);

  if ((this->read_task_handle_ == nullptr) && (this->decode_task_handle_ == nullptr)) {
    xEventGroupClearBits(this->event_group_, EventGroupBits::PIPELINE_COMMAND_STOP);
    this->is_playing_ = false;
    return AudioPipelineState::STOPPED;
  }

  if (event_bits & EventGroupBits::PIPELINE_COMMAND_STOP) {
    return AudioPipelineState::STOPPING;
  }

  if (this->pause_state_) {
    return AudioPipelineState::PAUSED;
  }

  this->is_playing_ = true;
  return AudioPipelineState::PLAYING;
}
```

#### Step 6: Compile

```bash
esphome compile config/satellite1.yaml
```

Expected: compiles cleanly.

#### Step 7: Run clang-format

```bash
clang-format -i esphome/components/speaker/media_player/audio_pipeline.cpp
```

#### Step 8: Commit

```bash
git add esphome/components/speaker/media_player/audio_pipeline.h \
        esphome/components/speaker/media_player/audio_pipeline.cpp
git commit -m "refactor(audio_pipeline): decompose process_state() into focused helpers"
```

---

### Task 4: Decompose `watch_media_commands_()` into helpers

**Files:**
- Modify: `esphome/components/speaker/media_player/speaker_media_player.h`
- Modify: `esphome/components/speaker/media_player/speaker_media_player.cpp`

#### Step 1: Add helper declarations to the header

In `speaker_media_player.h`, inside the `protected:` section, after `watch_media_commands_()`, add:

```cpp
  /// @brief Routes a play command (url or file) to the correct playlist, stopping the pipeline if not enqueuing.
  void handle_play_item_(MediaCallCommand &cmd);

  /// @brief Dispatches a transport command (play/pause/stop/toggle/mute/volume/repeat/clear).
  void handle_transport_command_(const MediaCallCommand &cmd);
```

Note: `handle_play_item_` takes a non-const ref because it deletes the heap-allocated URL string.

#### Step 2: Implement `handle_play_item_()`

Add to `speaker_media_player.cpp` (before `watch_media_commands_()`):

```cpp
void SpeakerMediaPlayer::handle_play_item_(MediaCallCommand &cmd) {
  bool enqueue = cmd.enqueue.has_value() && cmd.enqueue.value();

  PlaylistItem playlist_item;
  if (cmd.url.has_value()) {
    playlist_item.url = *cmd.url.value();
    delete cmd.url.value();
  }
  if (cmd.file.has_value()) {
    playlist_item.file = cmd.file.value();
  }

  if (this->single_pipeline_() || (cmd.announce.has_value() && cmd.announce.value())) {
    if (!enqueue) {
      this->cancel_timeout("next_ann");
      this->announcement_playlist_.clear();
      this->announcement_pipeline_->set_pause_state(false);
      this->announcement_pipeline_->stop();
    }
    this->announcement_playlist_.push_back(playlist_item);
  } else {
    if (!enqueue) {
      this->cancel_timeout("next_media");
      this->media_playlist_.clear();
      if (this->is_paused_) {
        this->media_pipeline_->stop();
        this->set_retry("unpause_med", 50, 3, [this](const uint8_t remaining_attempts) {
          if (this->media_pipeline_state_ == AudioPipelineState::STOPPED) {
            this->media_pipeline_->set_pause_state(false);
            this->is_paused_ = false;
            return RetryResult::DONE;
          }
          return RetryResult::RETRY;
        });
      }
      this->media_pipeline_->stop();
    }
    this->media_playlist_.push_back(playlist_item);
  }
}
```

#### Step 3: Implement `handle_transport_command_()`

```cpp
void SpeakerMediaPlayer::handle_transport_command_(const MediaCallCommand &cmd) {
  switch (cmd.command.value()) {
    case media_player::MEDIA_PLAYER_COMMAND_PLAY:
      if ((this->media_pipeline_ != nullptr) && (this->is_paused_)) {
        this->media_pipeline_->set_pause_state(false);
      }
      this->is_paused_ = false;
      break;
    case media_player::MEDIA_PLAYER_COMMAND_PAUSE:
    case media_player::MEDIA_PLAYER_COMMAND_STOP:
      if (this->single_pipeline_() || (cmd.announce.has_value() && cmd.announce.value())) {
        if (this->announcement_pipeline_ != nullptr) {
          this->cancel_timeout("next_ann");
          this->announcement_playlist_.clear();
          this->announcement_pipeline_->stop();
          this->set_retry("unpause_ann", 50, 3, [this](const uint8_t remaining_attempts) {
            if (this->announcement_pipeline_state_ == AudioPipelineState::STOPPED) {
              this->announcement_pipeline_->set_pause_state(false);
              return RetryResult::DONE;
            }
            return RetryResult::RETRY;
          });
        }
      } else {
        if (this->media_pipeline_ != nullptr) {
          this->cancel_timeout("next_media");
          this->media_playlist_.clear();
          this->media_pipeline_->stop();
          this->set_retry("unpause_med", 50, 3, [this](const uint8_t remaining_attempts) {
            if (this->media_pipeline_state_ == AudioPipelineState::STOPPED) {
              this->media_pipeline_->set_pause_state(false);
              this->is_paused_ = false;
              return RetryResult::DONE;
            }
            return RetryResult::RETRY;
          });
        }
      }
      break;
    case media_player::MEDIA_PLAYER_COMMAND_TOGGLE:
      if (this->media_pipeline_ != nullptr) {
        if (this->is_paused_) {
          this->media_pipeline_->set_pause_state(false);
          this->is_paused_ = false;
        } else {
          this->media_pipeline_->set_pause_state(true);
          this->is_paused_ = true;
        }
      }
      break;
    case media_player::MEDIA_PLAYER_COMMAND_MUTE:
      this->set_mute_state_(true);
      this->publish_state();
      break;
    case media_player::MEDIA_PLAYER_COMMAND_UNMUTE:
      this->set_mute_state_(false);
      this->publish_state();
      break;
    case media_player::MEDIA_PLAYER_COMMAND_VOLUME_UP:
      this->set_volume_(std::min(1.0f, this->volume + this->volume_increment_));
      this->publish_state();
      break;
    case media_player::MEDIA_PLAYER_COMMAND_VOLUME_DOWN:
      this->set_volume_(std::max(0.0f, this->volume - this->volume_increment_));
      this->publish_state();
      break;
    case media_player::MEDIA_PLAYER_COMMAND_REPEAT_ONE:
      if (this->single_pipeline_() || (cmd.announce.has_value() && cmd.announce.value())) {
        this->announcement_repeat_one_ = true;
      } else {
        this->media_repeat_one_ = true;
      }
      break;
    case media_player::MEDIA_PLAYER_COMMAND_REPEAT_OFF:
      if (this->single_pipeline_() || (cmd.announce.has_value() && cmd.announce.value())) {
        this->announcement_repeat_one_ = false;
      } else {
        this->media_repeat_one_ = false;
      }
      break;
    case media_player::MEDIA_PLAYER_COMMAND_CLEAR_PLAYLIST:
      if (this->single_pipeline_() || (cmd.announce.has_value() && cmd.announce.value())) {
        if (this->announcement_playlist_.empty()) {
          this->announcement_playlist_.resize(1);
        }
      } else {
        if (this->media_playlist_.empty()) {
          this->media_playlist_.resize(1);
        }
      }
      break;
    default:
      break;
  }
}
```

#### Step 4: Replace `watch_media_commands_()` body

```cpp
void SpeakerMediaPlayer::watch_media_commands_() {
  if (!this->is_ready()) {
    return;
  }

  MediaCallCommand media_command;
  if (xQueueReceive(this->media_control_command_queue_, &media_command, 0) != pdTRUE) {
    return;
  }

  if (media_command.url.has_value() || media_command.file.has_value()) {
    this->handle_play_item_(media_command);
    return;
  }

  if (media_command.volume.has_value()) {
    this->set_volume_(media_command.volume.value());
    this->publish_state();
    return;
  }

  if (media_command.command.has_value()) {
    this->handle_transport_command_(media_command);
  }
}
```

#### Step 5: Compile

```bash
esphome compile config/satellite1.yaml
```

Expected: compiles cleanly.

#### Step 6: Run clang-format

```bash
clang-format -i esphome/components/speaker/media_player/speaker_media_player.h
clang-format -i esphome/components/speaker/media_player/speaker_media_player.cpp
```

#### Step 7: Commit

```bash
git add esphome/components/speaker/media_player/speaker_media_player.h \
        esphome/components/speaker/media_player/speaker_media_player.cpp
git commit -m "refactor(speaker_media_player): decompose watch_media_commands_() into focused helpers"
```

---

### Task 5: Final verification and lint

**Files:** none (verification only)

#### Step 1: Compile all three configs

```bash
esphome compile config/satellite1.yaml
esphome compile config/satellite1.ld2410.yaml
esphome compile config/satellite1.ld2450.yaml
```

Expected: all three compile cleanly.

#### Step 2: Run pre-commit hooks on all changed files

```bash
pre-commit run --all-files
```

Expected: clang-format and yamllint both pass with no violations. If clang-format reports changes, apply them, re-compile, and commit the fixes.

#### Step 3: Final commit if pre-commit made any fixes

```bash
git add esphome/components/speaker/media_player/
git commit -m "style: apply clang-format to audio pipeline refactor"
```

#### Step 4: Verify git log

```bash
git log --oneline -6
```

Expected output (roughly):
```
<hash> style: apply clang-format to audio pipeline refactor  (if needed)
<hash> refactor(speaker_media_player): decompose watch_media_commands_() into focused helpers
<hash> refactor(audio_pipeline): decompose process_state() into focused helpers
<hash> refactor(audio_pipeline): consolidate source storage into PipelineSource struct
<hash> docs: add audio pipeline refactor design doc
```
