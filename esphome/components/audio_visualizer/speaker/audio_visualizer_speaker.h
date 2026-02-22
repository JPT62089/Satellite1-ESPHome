#pragma once
#ifdef USE_ESP32

#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace esphome {
namespace audio_visualizer {

static const uint32_t FFT_SIZE = 512;
static const uint32_t NUM_BANDS = 24;
static const float ALPHA_BANDS = 0.3f;
static const float ALPHA_RMS = 0.6f;
static const float BEAT_THRESHOLD = 1.5f;
static const uint32_t BEAT_HISTORY_LEN = 50;

class AudioVisualizerSpeaker : public Component, public speaker::Speaker {
 public:
  void setup() override;

  size_t play(const uint8_t *data, size_t length) override;
  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait, bool write_partial = false) override {
    return this->play(data, length);
  }
  size_t play_silence(size_t length_ms) override {
    return this->output_speaker_ ? this->output_speaker_->play_silence(length_ms) : 0;
  }

  void start() override;
  void stop() override;
  void finish() override;
  bool has_buffered_data() const override;
  bool is_stopped() const override;

  void set_mute_state(bool mute) override;
  bool get_mute_state() override;
  void set_volume(float volume) override;
  float get_volume() override;
  void set_pause_state(bool pause) override;
  bool get_pause_state() const override;
  int64_t get_playout_time(int64_t self_buffer_us) const override;
  bool update_buffer_states(int32_t bytes_transferred) override;

  void set_output_speaker(speaker::Speaker *speaker) { this->output_speaker_ = speaker; }

  // Analysis results — read by LED effects
  float get_rms() const { return this->rms_.load(); }
  /// Copies the current 24 band energies into @p out.
  /// @p out must be pre-initialized (e.g. to zeros) — if the mutex is
  /// contended, @p out is left unmodified and the caller retains its
  /// previous values.
  void get_bands(float out[NUM_BANDS]) const;
  bool consume_beat() { return this->beat_.exchange(false); }

 protected:
  void accumulate_samples_(const int16_t *samples, size_t num_stereo_frames);
  void analyze_window_();

  speaker::Speaker *output_speaker_{nullptr};

  // Sample accumulation (mono, float)
  float sample_window_[FFT_SIZE]{};
  uint32_t sample_count_{0};

  // Published analysis results
  std::atomic<float> rms_{0.0f};
  float bands_[NUM_BANDS]{};
  mutable SemaphoreHandle_t bands_mutex_{nullptr};
  std::atomic<bool> beat_{false};

  // Internal smoothing state
  float smoothed_bands_[NUM_BANDS]{};
  float smoothed_rms_{0.0f};

  // Beat detection rolling average
  float bass_history_[BEAT_HISTORY_LEN]{};
  uint32_t bass_history_idx_{0};

  // Band bin ranges precomputed in setup()
  uint16_t band_bin_start_[NUM_BANDS]{};
  uint16_t band_bin_end_[NUM_BANDS]{};

  // Diagnostic logging state
  uint32_t window_count_{0};
  bool first_data_logged_{false};
};

}  // namespace audio_visualizer
}  // namespace esphome

#endif  // USE_ESP32
