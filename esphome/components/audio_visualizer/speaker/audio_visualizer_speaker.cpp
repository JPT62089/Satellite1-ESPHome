#ifdef USE_ESP32

#include "audio_visualizer_speaker.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace esphome {
namespace audio_visualizer {

static const char *const TAG = "audio_visualizer";

void AudioVisualizerSpeaker::setup() {
  this->bands_mutex_ = xSemaphoreCreateMutex();
  if (this->bands_mutex_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create bands mutex");
    this->mark_failed();
    return;
  }

  // Precompute logarithmic band bin ranges (60 Hz – 16 kHz over NUM_BANDS bands)
  const float f_low = 60.0f;
  const float f_high = 16000.0f;
  const float bin_hz = 48000.0f / FFT_SIZE;

  for (uint32_t b = 0; b < NUM_BANDS; b++) {
    float f_start = f_low * powf(f_high / f_low, (float) b / NUM_BANDS);
    float f_end = f_low * powf(f_high / f_low, (float) (b + 1) / NUM_BANDS);
    this->band_bin_start_[b] = (uint16_t) std::max(1.0f, f_start / bin_hz);
    this->band_bin_end_[b] = (uint16_t) std::max(f_end / bin_hz, (float) (this->band_bin_start_[b] + 1));
    this->band_bin_end_[b] = std::min(this->band_bin_end_[b], (uint16_t) (FFT_SIZE / 2 - 1));
  }
}

size_t AudioVisualizerSpeaker::play(const uint8_t *data, size_t length) {
  // Forward first — audio path is never blocked by analysis
  size_t written = 0;
  if (this->output_speaker_) {
    written = this->output_speaker_->play(data, length);
  }
  // Accumulate for analysis (16-bit stereo → mono)
  const auto *samples = reinterpret_cast<const int16_t *>(data);
  size_t num_frames = length / 4;  // 2 ch × 2 bytes
  this->accumulate_samples_(samples, num_frames);
  return written;
}

void AudioVisualizerSpeaker::start() {
  this->state_ = speaker::STATE_STARTING;
  if (this->output_speaker_) {
    this->output_speaker_->set_audio_stream_info(this->audio_stream_info_);
    this->output_speaker_->start();
  }
  this->state_ = speaker::STATE_RUNNING;
}

void AudioVisualizerSpeaker::stop() {
  this->state_ = speaker::STATE_STOPPING;
  if (this->output_speaker_)
    this->output_speaker_->stop();
  this->state_ = speaker::STATE_STOPPED;
  this->rms_.store(0.0f);
  this->beat_.store(false);
}

void AudioVisualizerSpeaker::finish() {
  if (this->output_speaker_)
    this->output_speaker_->finish();
}

bool AudioVisualizerSpeaker::has_buffered_data() const {
  return this->output_speaker_ && this->output_speaker_->has_buffered_data();
}

bool AudioVisualizerSpeaker::is_stopped() const {
  return this->state_ == speaker::STATE_STOPPED && (!this->output_speaker_ || this->output_speaker_->is_stopped());
}

void AudioVisualizerSpeaker::set_mute_state(bool mute) {
  this->mute_state_ = mute;
  if (this->output_speaker_)
    this->output_speaker_->set_mute_state(mute);
}
bool AudioVisualizerSpeaker::get_mute_state() {
  return this->output_speaker_ ? this->output_speaker_->get_mute_state() : this->mute_state_;
}
void AudioVisualizerSpeaker::set_volume(float volume) {
  this->volume_ = volume;
  if (this->output_speaker_)
    this->output_speaker_->set_volume(volume);
}
float AudioVisualizerSpeaker::get_volume() {
  return this->output_speaker_ ? this->output_speaker_->get_volume() : this->volume_;
}
void AudioVisualizerSpeaker::set_pause_state(bool pause) {
  if (this->output_speaker_)
    this->output_speaker_->set_pause_state(pause);
}
bool AudioVisualizerSpeaker::get_pause_state() const {
  return this->output_speaker_ && this->output_speaker_->get_pause_state();
}
int64_t AudioVisualizerSpeaker::get_playout_time(int64_t self_buffer_us) const {
  return this->output_speaker_ ? this->output_speaker_->get_playout_time(self_buffer_us) : 0;
}
bool AudioVisualizerSpeaker::update_buffer_states(int32_t bytes_transferred) {
  return !this->output_speaker_ || this->output_speaker_->update_buffer_states(bytes_transferred);
}

void AudioVisualizerSpeaker::get_bands(float out[NUM_BANDS]) const {
  if (xSemaphoreTake(this->bands_mutex_, 0) == pdTRUE) {
    memcpy(out, this->bands_, NUM_BANDS * sizeof(float));
    xSemaphoreGive(this->bands_mutex_);
  }
}

void AudioVisualizerSpeaker::accumulate_samples_(const int16_t *samples, size_t num_frames) {
  for (size_t i = 0; i < num_frames; i++) {
    float mono = (samples[i * 2] + samples[i * 2 + 1]) * 0.5f / 32768.0f;
    this->sample_window_[this->sample_count_++] = mono;
    if (this->sample_count_ >= FFT_SIZE) {
      this->analyze_window_();
      // 50% overlap: keep the second half for the next window
      memmove(this->sample_window_, this->sample_window_ + FFT_SIZE / 2, (FFT_SIZE / 2) * sizeof(float));
      this->sample_count_ = FFT_SIZE / 2;
    }
  }
}

void AudioVisualizerSpeaker::analyze_window_() {
  // Stub — implemented in Task 2
}

}  // namespace audio_visualizer
}  // namespace esphome

#endif  // USE_ESP32
