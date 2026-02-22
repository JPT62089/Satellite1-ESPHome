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
  this->sample_count_ = 0;
  memset(this->sample_window_, 0, sizeof(this->sample_window_));
  this->smoothed_rms_ = 0.0f;
  memset(this->smoothed_bands_, 0, sizeof(this->smoothed_bands_));
  memset(this->bands_, 0, sizeof(this->bands_));
  memset(this->bass_history_, 0, sizeof(this->bass_history_));
  this->bass_history_idx_ = 0;
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

// Iterative Cooley-Tukey radix-2 in-place FFT (N must be power of 2)
static void fft_compute(float *re, float *im, uint32_t n) {
  // Bit-reversal permutation
  for (uint32_t i = 1, j = 0; i < n; i++) {
    uint32_t bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j) {
      std::swap(re[i], re[j]);
      std::swap(im[i], im[j]);
    }
  }
  // Butterfly stages
  for (uint32_t len = 2; len <= n; len <<= 1) {
    float ang = -2.0f * (float) M_PI / len;
    float wr0 = cosf(ang), wi0 = sinf(ang);
    for (uint32_t i = 0; i < n; i += len) {
      float wr = 1.0f, wi = 0.0f;
      for (uint32_t j = 0; j < len / 2; j++) {
        float ur = re[i + j], ui = im[i + j];
        float vr = re[i + j + len / 2] * wr - im[i + j + len / 2] * wi;
        float vi = re[i + j + len / 2] * wi + im[i + j + len / 2] * wr;
        re[i + j] = ur + vr;
        im[i + j] = ui + vi;
        re[i + j + len / 2] = ur - vr;
        im[i + j + len / 2] = ui - vi;
        float new_wr = wr * wr0 - wi * wi0;
        wi = wr * wi0 + wi * wr0;
        wr = new_wr;
      }
    }
  }
}

void AudioVisualizerSpeaker::analyze_window_() {
  // 1. RMS from raw samples (before windowing — more accurate loudness)
  float sum_sq = 0.0f;
  for (uint32_t i = 0; i < FFT_SIZE; i++)
    sum_sq += this->sample_window_[i] * this->sample_window_[i];
  float raw_rms = sqrtf(sum_sq / FFT_SIZE);
  this->smoothed_rms_ = ALPHA_RMS * raw_rms + (1.0f - ALPHA_RMS) * this->smoothed_rms_;
  // Scale: typical music RMS ~0.1-0.25 → map to 0-1
  this->rms_.store(std::min(1.0f, this->smoothed_rms_ * 5.0f));

  // 2. Apply Hann window to sample buffer → FFT buffers
  // Static buffers to avoid 4 KB stack allocation.
  // Safe: single AudioVisualizerSpeaker instance, called only from the audio task.
  static float fft_re[FFT_SIZE];
  static float fft_im[FFT_SIZE];
  for (uint32_t i = 0; i < FFT_SIZE; i++) {
    float w = 0.5f * (1.0f - cosf(2.0f * (float) M_PI * i / (FFT_SIZE - 1)));
    fft_re[i] = this->sample_window_[i] * w;
    fft_im[i] = 0.0f;
  }

  // 3. Run FFT (~50 µs on ESP32-S3 @ 240 MHz)
  fft_compute(fft_re, fft_im, FFT_SIZE);

  // 4. Compute magnitude for positive frequency bins [1 .. FFT_SIZE/2-1]
  static float magnitudes[FFT_SIZE / 2];
  for (uint32_t i = 0; i < FFT_SIZE / 2; i++) {
    magnitudes[i] = sqrtf(fft_re[i] * fft_re[i] + fft_im[i] * fft_im[i]);
  }

  // 5. Average bins into 24 log bands + smooth
  float new_bands[NUM_BANDS];
  for (uint32_t b = 0; b < NUM_BANDS; b++) {
    float sum = 0.0f;
    for (uint32_t k = this->band_bin_start_[b]; k <= this->band_bin_end_[b]; k++) {
      sum += magnitudes[k];
    }
    uint32_t count = this->band_bin_end_[b] - this->band_bin_start_[b] + 1;
    // Normalize: divisor ~20 works for typical music; tune if too dim/bright
    new_bands[b] = std::min(1.0f, (sum / count) / 20.0f);
    this->smoothed_bands_[b] = ALPHA_BANDS * new_bands[b] + (1.0f - ALPHA_BANDS) * this->smoothed_bands_[b];
  }

  if (xSemaphoreTake(this->bands_mutex_, 0) == pdTRUE) {
    memcpy(this->bands_, this->smoothed_bands_, NUM_BANDS * sizeof(float));
    xSemaphoreGive(this->bands_mutex_);
  }

  // 6. Beat detection: bass band vs rolling average
  float bass_energy = (this->smoothed_bands_[0] + this->smoothed_bands_[1]) * 0.5f;
  this->bass_history_[this->bass_history_idx_] = bass_energy;
  this->bass_history_idx_ = (this->bass_history_idx_ + 1) % BEAT_HISTORY_LEN;

  float avg = 0.0f;
  for (uint32_t i = 0; i < BEAT_HISTORY_LEN; i++)
    avg += this->bass_history_[i];
  avg /= BEAT_HISTORY_LEN;

  if (avg > 0.01f && bass_energy > BEAT_THRESHOLD * avg) {
    this->beat_.store(true);
  }
}

}  // namespace audio_visualizer
}  // namespace esphome

#endif  // USE_ESP32
