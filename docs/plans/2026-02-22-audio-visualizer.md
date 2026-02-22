# Audio Visualizer Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a Winamp-style audio-reactive LED ring visualizer driven by the live media playback PCM stream, with 4 presets and Home Assistant controls.

**Architecture:** A new `AudioVisualizerSpeaker` pass-through component sits between `media_resampling_speaker` and `media_mixing_input`, tapping 16-bit stereo PCM to compute RMS amplitude, a 24-band log-spectrum via 512-pt FFT, and beat detection. Four `AddressableLightEffect` subclasses read that shared data and animate the 24-LED ring at ~30fps. The existing `control_leds` priority script is extended with a new branch that activates the visualizer when media is playing and the HA switch is on.

**Tech Stack:** ESPHome custom C++ component (ESP-IDF, FreeRTOS), ESPHome `speaker::Speaker` + `light::AddressableLightEffect` APIs, self-contained iterative Cooley-Tukey FFT (no external deps), YAML configuration.

---

### Task 1: `AudioVisualizerSpeaker` pass-through skeleton

**Files:**
- Create: `esphome/components/audio_visualizer/__init__.py`
- Create: `esphome/components/audio_visualizer/speaker/__init__.py`
- Create: `esphome/components/audio_visualizer/speaker/audio_visualizer_speaker.h`
- Create: `esphome/components/audio_visualizer/speaker/audio_visualizer_speaker.cpp`

A fully-functional pass-through speaker that forwards all PCM data unchanged — no analysis yet. This establishes the component structure and delegation pattern.

**Step 1: Create top-level `__init__.py` (empty)**

```python
# esphome/components/audio_visualizer/__init__.py
```

**Step 2: Create `esphome/components/audio_visualizer/speaker/__init__.py`**

```python
import esphome.codegen as cg
from esphome.components import esp32, speaker
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_OUTPUT_SPEAKER,
    PLATFORM_ESP32,
)

AUTO_LOAD = ["audio"]
DEPENDENCIES = ["speaker"]

audio_visualizer_ns = cg.esphome_ns.namespace("audio_visualizer")
AudioVisualizerSpeaker = audio_visualizer_ns.class_(
    "AudioVisualizerSpeaker", cg.Component, speaker.Speaker
)

CONF_LIGHT = "light"

CONFIG_SCHEMA = cv.All(
    speaker.SPEAKER_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(AudioVisualizerSpeaker),
            cv.Required(CONF_OUTPUT_SPEAKER): cv.use_id(speaker.Speaker),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32]),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)

    output_spkr = await cg.get_variable(config[CONF_OUTPUT_SPEAKER])
    cg.add(var.set_output_speaker(output_spkr))
```

(The `light` parameter and effect codegen are added in Task 4.)

**Step 3: Create `audio_visualizer_speaker.h`**

```cpp
// esphome/components/audio_visualizer/speaker/audio_visualizer_speaker.h
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
  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait,
              bool write_partial = false) override {
    return this->play(data, length);
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
  void get_bands(float out[NUM_BANDS]) const;
  bool consume_beat() { return this->beat_.exchange(false); }

 protected:
  void accumulate_samples_(const int16_t *samples, size_t num_stereo_frames);
  void analyze_window_();

  speaker::Speaker *output_speaker_{nullptr};

  // Sample accumulation (mono, float)
  float sample_window_[FFT_SIZE];
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
};

}  // namespace audio_visualizer
}  // namespace esphome

#endif  // USE_ESP32
```

**Step 4: Create `audio_visualizer_speaker.cpp` (pass-through, stub analysis)**

```cpp
// esphome/components/audio_visualizer/speaker/audio_visualizer_speaker.cpp
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

  // Precompute logarithmic band bin ranges (60 Hz – 16 kHz over NUM_BANDS bands)
  const float f_low = 60.0f;
  const float f_high = 16000.0f;
  const float bin_hz = 48000.0f / FFT_SIZE;

  for (uint32_t b = 0; b < NUM_BANDS; b++) {
    float f_start = f_low * powf(f_high / f_low, (float) b / NUM_BANDS);
    float f_end   = f_low * powf(f_high / f_low, (float)(b + 1) / NUM_BANDS);
    this->band_bin_start_[b] = (uint16_t) std::max(1.0f, f_start / bin_hz);
    this->band_bin_end_[b]   = (uint16_t) std::max(f_end / bin_hz,
                                                    (float)(this->band_bin_start_[b] + 1));
    this->band_bin_end_[b]   = std::min(this->band_bin_end_[b], (uint16_t)(FFT_SIZE / 2 - 1));
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
  if (this->output_speaker_) this->output_speaker_->start();
  this->state_ = speaker::STATE_RUNNING;
}

void AudioVisualizerSpeaker::stop() {
  this->state_ = speaker::STATE_STOPPING;
  if (this->output_speaker_) this->output_speaker_->stop();
  this->state_ = speaker::STATE_STOPPED;
  this->rms_.store(0.0f);
  this->beat_.store(false);
}

void AudioVisualizerSpeaker::finish() {
  if (this->output_speaker_) this->output_speaker_->finish();
}

bool AudioVisualizerSpeaker::has_buffered_data() const {
  return this->output_speaker_ && this->output_speaker_->has_buffered_data();
}

bool AudioVisualizerSpeaker::is_stopped() const {
  return this->state_ == speaker::STATE_STOPPED &&
         (!this->output_speaker_ || this->output_speaker_->is_stopped());
}

void AudioVisualizerSpeaker::set_mute_state(bool mute) {
  this->mute_state_ = mute;
  if (this->output_speaker_) this->output_speaker_->set_mute_state(mute);
}
bool AudioVisualizerSpeaker::get_mute_state() {
  return this->output_speaker_ ? this->output_speaker_->get_mute_state() : this->mute_state_;
}
void AudioVisualizerSpeaker::set_volume(float volume) {
  this->volume_ = volume;
  if (this->output_speaker_) this->output_speaker_->set_volume(volume);
}
float AudioVisualizerSpeaker::get_volume() {
  return this->output_speaker_ ? this->output_speaker_->get_volume() : this->volume_;
}
void AudioVisualizerSpeaker::set_pause_state(bool pause) {
  if (this->output_speaker_) this->output_speaker_->set_pause_state(pause);
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
      memmove(this->sample_window_, this->sample_window_ + FFT_SIZE / 2,
              (FFT_SIZE / 2) * sizeof(float));
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
```

**Step 5: Check formatting**

```bash
clang-format --dry-run \
  esphome/components/audio_visualizer/speaker/audio_visualizer_speaker.h \
  esphome/components/audio_visualizer/speaker/audio_visualizer_speaker.cpp
```

Expected: no output (no formatting errors). Fix any reported lines before committing.

**Step 6: Commit**

```bash
git add esphome/components/audio_visualizer/
git commit -m "feat(audio_visualizer): add AudioVisualizerSpeaker pass-through skeleton"
```

---

### Task 2: FFT and signal analysis

**Files:**
- Modify: `esphome/components/audio_visualizer/speaker/audio_visualizer_speaker.cpp`

Replace the stub `analyze_window_()` with real FFT-based signal analysis: RMS amplitude, 24-band log-spectrum, exponential smoothing, and beat detection.

**Step 1: Add the FFT helper above `analyze_window_()`**

Insert this static function in `audio_visualizer_speaker.cpp`, before `analyze_window_()`:

```cpp
// Iterative Cooley-Tukey radix-2 in-place FFT (N must be power of 2)
static void fft_compute(float *re, float *im, uint32_t n) {
  // Bit-reversal permutation
  for (uint32_t i = 1, j = 0; i < n; i++) {
    uint32_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
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
        re[i + j]           = ur + vr;
        im[i + j]           = ui + vi;
        re[i + j + len / 2] = ur - vr;
        im[i + j + len / 2] = ui - vi;
        float new_wr = wr * wr0 - wi * wi0;
        wi = wr * wi0 + wi * wr0;
        wr = new_wr;
      }
    }
  }
}
```

**Step 2: Replace the stub `analyze_window_()` with the full implementation**

```cpp
void AudioVisualizerSpeaker::analyze_window_() {
  // 1. RMS from raw samples (before windowing — more accurate loudness)
  float sum_sq = 0.0f;
  for (uint32_t i = 0; i < FFT_SIZE; i++) sum_sq += this->sample_window_[i] * this->sample_window_[i];
  float raw_rms = sqrtf(sum_sq / FFT_SIZE);
  this->smoothed_rms_ = ALPHA_RMS * raw_rms + (1.0f - ALPHA_RMS) * this->smoothed_rms_;
  // Scale: typical music RMS ~0.1-0.25 → map to 0-1
  this->rms_.store(std::min(1.0f, this->smoothed_rms_ * 5.0f));

  // 2. Apply Hann window to sample buffer → FFT buffers
  // Use static buffers to avoid stack allocation of 512*8 = 4KB
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
  for (uint32_t i = 0; i < BEAT_HISTORY_LEN; i++) avg += this->bass_history_[i];
  avg /= BEAT_HISTORY_LEN;

  if (avg > 0.01f && bass_energy > BEAT_THRESHOLD * avg) {
    this->beat_.store(true);
  }
}
```

**Step 3: Check formatting**

```bash
clang-format --dry-run esphome/components/audio_visualizer/speaker/audio_visualizer_speaker.cpp
```

**Step 4: Commit**

```bash
git add esphome/components/audio_visualizer/speaker/audio_visualizer_speaker.cpp
git commit -m "feat(audio_visualizer): implement FFT analysis with RMS, log bands, and beat detection"
```

---

### Task 3: Four LED visualizer effects

**Files:**
- Create: `esphome/components/audio_visualizer/speaker/visualizer_effect.h`
- Create: `esphome/components/audio_visualizer/speaker/visualizer_effect.cpp`

**Step 1: Create `visualizer_effect.h`**

```cpp
// esphome/components/audio_visualizer/speaker/visualizer_effect.h
#pragma once
#ifdef USE_ESP32

#include "audio_visualizer_speaker.h"
#include "esphome/components/light/addressable_light_effect.h"

#include <cmath>

namespace esphome {
namespace audio_visualizer {

// HSV → ESPHome Color helper (h, s, v all in [0, 1])
inline light::Color hsv_to_color(float h, float s, float v) {
  h = fmodf(h, 1.0f);
  float r = 0, g = 0, b = 0;
  int i = (int)(h * 6);
  float f = h * 6.0f - i;
  float p = v * (1.0f - s);
  float q = v * (1.0f - f * s);
  float t = v * (1.0f - (1.0f - f) * s);
  switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    case 5: r = v; g = p; b = q; break;
  }
  return light::Color((uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255));
}

// Base class — all presets hold a reference to the visualizer speaker
class VisualizerEffect : public light::AddressableLightEffect {
 public:
  explicit VisualizerEffect(const std::string &name) : AddressableLightEffect(name) {}
  void set_visualizer(AudioVisualizerSpeaker *viz) { this->viz_ = viz; }

 protected:
  AudioVisualizerSpeaker *viz_{nullptr};
};

// Preset 1: Spectrum Ring
// Each LED = one frequency band; color gradient blue (bass) → red (treble)
class SpectrumRingEffect : public VisualizerEffect {
 public:
  explicit SpectrumRingEffect(const std::string &name) : VisualizerEffect(name) {}
  void apply(light::AddressableLight &it, const light::Color &current_color) override;
};

// Preset 2: Pulse / Beat
// All LEDs pulse with RMS; flashes white on beat
class PulseBeatEffect : public VisualizerEffect {
 public:
  explicit PulseBeatEffect(const std::string &name) : VisualizerEffect(name) {}
  void apply(light::AddressableLight &it, const light::Color &current_color) override;

 protected:
  float smoothed_{0.0f};
  float beat_flash_{0.0f};
};

// Preset 3: VU Sweep
// LEDs fill clockwise with RMS; green → yellow → red gradient
class VUSweepEffect : public VisualizerEffect {
 public:
  explicit VUSweepEffect(const std::string &name) : VisualizerEffect(name) {}
  void apply(light::AddressableLight &it, const light::Color &current_color) override;
};

// Preset 4: Waveform Orbit
// RMS history orbits the ring: new energy enters at LED 0 and chases around
class WaveformOrbitEffect : public VisualizerEffect {
 public:
  explicit WaveformOrbitEffect(const std::string &name) : VisualizerEffect(name) {}
  void apply(light::AddressableLight &it, const light::Color &current_color) override;

 protected:
  float history_[NUM_BANDS]{};
  uint32_t head_{0};
};

}  // namespace audio_visualizer
}  // namespace esphome

#endif  // USE_ESP32
```

**Step 2: Create `visualizer_effect.cpp`**

```cpp
// esphome/components/audio_visualizer/speaker/visualizer_effect.cpp
#ifdef USE_ESP32

#include "visualizer_effect.h"

#include <algorithm>
#include <cmath>

namespace esphome {
namespace audio_visualizer {

// --- Preset 1: Spectrum Ring ---
void SpectrumRingEffect::apply(light::AddressableLight &it, const light::Color &current_color) {
  if (!this->viz_) return;
  float bands[NUM_BANDS];
  this->viz_->get_bands(bands);
  for (int i = 0; i < it.size() && i < (int) NUM_BANDS; i++) {
    // Hue sweeps from 0.67 (blue) at bass to 0.0 (red) at treble
    float hue = 0.67f * (1.0f - (float) i / (NUM_BANDS - 1));
    float brightness = std::min(1.0f, bands[i]);
    it[i] = hsv_to_color(hue, 1.0f, brightness);
  }
}

// --- Preset 2: Pulse / Beat ---
void PulseBeatEffect::apply(light::AddressableLight &it, const light::Color &current_color) {
  if (!this->viz_) return;
  float rms = this->viz_->get_rms();
  bool beat = this->viz_->consume_beat();

  // Asymmetric envelope: fast attack, slow decay
  if (rms > this->smoothed_) {
    this->smoothed_ = 0.8f * rms + 0.2f * this->smoothed_;
  } else {
    this->smoothed_ = 0.1f * rms + 0.9f * this->smoothed_;
  }

  if (beat) this->beat_flash_ = 1.0f;
  this->beat_flash_ *= 0.75f;  // decay flash over several frames

  float brightness = std::min(1.0f, this->smoothed_ + this->beat_flash_ * 0.5f);
  // Desaturate toward white on beat flash
  float sat = 1.0f - this->beat_flash_ * 0.7f;
  light::Color c = hsv_to_color(0.57f, sat, brightness);  // sky-blue base
  for (int i = 0; i < it.size(); i++) it[i] = c;
}

// --- Preset 3: VU Sweep ---
void VUSweepEffect::apply(light::AddressableLight &it, const light::Color &current_color) {
  if (!this->viz_) return;
  float rms = this->viz_->get_rms();
  int lit = (int)(rms * it.size() + 0.5f);
  lit = std::max(0, std::min(it.size(), lit));
  for (int i = 0; i < it.size(); i++) {
    if (i < lit) {
      float t = (float) i / (it.size() - 1);
      light::Color c;
      if (t < 0.5f) {
        // green → yellow
        c = light::Color((uint8_t)(t * 2.0f * 255), 255, 0);
      } else {
        // yellow → red
        c = light::Color(255, (uint8_t)((1.0f - (t - 0.5f) * 2.0f) * 255), 0);
      }
      it[i] = c;
    } else {
      it[i] = light::Color(0, 0, 0);
    }
  }
}

// --- Preset 4: Waveform Orbit ---
void WaveformOrbitEffect::apply(light::AddressableLight &it, const light::Color &current_color) {
  if (!this->viz_) return;
  float rms = this->viz_->get_rms();

  // Push new sample into the circular history buffer
  this->history_[this->head_] = rms;
  this->head_ = (this->head_ + 1) % NUM_BANDS;

  // LED 0 = most recent value, LED 23 = oldest
  for (int i = 0; i < it.size() && i < (int) NUM_BANDS; i++) {
    uint32_t idx = (this->head_ + NUM_BANDS - 1 - i) % NUM_BANDS;
    float brightness = std::min(1.0f, this->history_[idx]);
    it[i] = hsv_to_color(0.57f, 1.0f, brightness);
  }
}

}  // namespace audio_visualizer
}  // namespace esphome

#endif  // USE_ESP32
```

**Step 3: Check formatting**

```bash
clang-format --dry-run \
  esphome/components/audio_visualizer/speaker/visualizer_effect.h \
  esphome/components/audio_visualizer/speaker/visualizer_effect.cpp
```

**Step 4: Commit**

```bash
git add esphome/components/audio_visualizer/speaker/visualizer_effect.h \
        esphome/components/audio_visualizer/speaker/visualizer_effect.cpp
git commit -m "feat(audio_visualizer): implement Spectrum, Pulse, VU Sweep, and Waveform Orbit effects"
```

---

### Task 4: Complete `__init__.py`, YAML config, and integration

**Files:**
- Modify: `esphome/components/audio_visualizer/speaker/__init__.py`
- Create: `config/common/audio_visualizer.yaml`
- Modify: `config/common/media_player.yaml`
- Modify: `config/common/led_ring.yaml`
- Modify: `config/satellite1.base.yaml`

**Step 1: Replace `speaker/__init__.py` with the full version (adds light + effect codegen)**

```python
import esphome.codegen as cg
from esphome.components import esp32, light, speaker
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_OUTPUT_SPEAKER,
    PLATFORM_ESP32,
)

AUTO_LOAD = ["audio"]
DEPENDENCIES = ["speaker", "light"]

audio_visualizer_ns = cg.esphome_ns.namespace("audio_visualizer")
AudioVisualizerSpeaker = audio_visualizer_ns.class_(
    "AudioVisualizerSpeaker", cg.Component, speaker.Speaker
)
SpectrumRingEffect = audio_visualizer_ns.class_(
    "SpectrumRingEffect", light.AddressableLightEffect
)
PulseBeatEffect = audio_visualizer_ns.class_(
    "PulseBeatEffect", light.AddressableLightEffect
)
VUSweepEffect = audio_visualizer_ns.class_(
    "VUSweepEffect", light.AddressableLightEffect
)
WaveformOrbitEffect = audio_visualizer_ns.class_(
    "WaveformOrbitEffect", light.AddressableLightEffect
)

CONF_LIGHT = "light"
CONF_SPECTRUM_ID = "spectrum_effect_id"
CONF_PULSE_ID = "pulse_effect_id"
CONF_VU_ID = "vu_effect_id"
CONF_WAVEFORM_ID = "waveform_effect_id"

CONFIG_SCHEMA = cv.All(
    speaker.SPEAKER_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(AudioVisualizerSpeaker),
            cv.Required(CONF_OUTPUT_SPEAKER): cv.use_id(speaker.Speaker),
            cv.Required(CONF_LIGHT): cv.use_id(light.LightState),
            cv.GenerateID(CONF_SPECTRUM_ID): cv.declare_id(SpectrumRingEffect),
            cv.GenerateID(CONF_PULSE_ID): cv.declare_id(PulseBeatEffect),
            cv.GenerateID(CONF_VU_ID): cv.declare_id(VUSweepEffect),
            cv.GenerateID(CONF_WAVEFORM_ID): cv.declare_id(WaveformOrbitEffect),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32]),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)

    output_spkr = await cg.get_variable(config[CONF_OUTPUT_SPEAKER])
    cg.add(var.set_output_speaker(output_spkr))

    light_var = await cg.get_variable(config[CONF_LIGHT])

    spectrum = cg.new_Pvariable(config[CONF_SPECTRUM_ID], "Visualizer Spectrum")
    cg.add(spectrum.set_visualizer(var))
    cg.add(spectrum.set_update_interval(33))

    pulse = cg.new_Pvariable(config[CONF_PULSE_ID], "Visualizer Pulse")
    cg.add(pulse.set_visualizer(var))
    cg.add(pulse.set_update_interval(33))

    vu = cg.new_Pvariable(config[CONF_VU_ID], "Visualizer VU Sweep")
    cg.add(vu.set_visualizer(var))
    cg.add(vu.set_update_interval(33))

    waveform = cg.new_Pvariable(config[CONF_WAVEFORM_ID], "Visualizer Waveform")
    cg.add(waveform.set_visualizer(var))
    cg.add(waveform.set_update_interval(33))

    cg.add(light_var.add_effects([spectrum, pulse, vu, waveform]))
```

**Step 2: Create `config/common/audio_visualizer.yaml`**

```yaml
speaker:
  - platform: audio_visualizer
    id: audio_viz
    output_speaker: media_mixing_input
    light: voice_assistant_leds

switch:
  - platform: template
    name: "Visualizer Enabled"
    id: visualizer_enabled
    icon: "mdi:equalizer"
    entity_category: config
    restore_mode: RESTORE_DEFAULT_ON
    optimistic: true
    on_turn_on:
      - script.execute: control_leds
    on_turn_off:
      - script.execute: control_leds

select:
  - platform: template
    name: "Visualizer Preset"
    id: visualizer_preset
    icon: "mdi:palette"
    entity_category: config
    options:
      - "Spectrum"
      - "Pulse"
      - "VU Sweep"
      - "Waveform"
    initial_option: "Spectrum"
    optimistic: true
    restore_value: true
    on_value:
      - if:
          condition:
            switch.is_on: visualizer_enabled
          then:
            - script.execute: control_leds
```

**Step 3: Update `config/common/media_player.yaml`**

Change `output_speaker` of `media_resampling_speaker` from `media_mixing_input` to `audio_viz` (line 162):

```yaml
  - platform: resampler
    id: media_resampling_speaker
    output_speaker: audio_viz          # was: media_mixing_input
    sample_rate: 48000
    bits_per_sample: 16
```

Also add `- script.execute: control_leds` to `on_state` so the LED state machine responds when playback starts/stops. The existing `on_state` block starts at line 202 — add the script call as a first action before the `if:`:

```yaml
    on_state:
      - script.execute: control_leds   # add this line
      - if:
          condition:
            and:
              - switch.is_off: timer_ringing
              ...  (existing conditions unchanged)
```

**Step 4: Update `config/common/led_ring.yaml` — add visualizer branch to `control_leds`**

In the `control_leds` lambda (around line 549), insert the visualizer branch between the timer-ticking block and the mute/volume blocks:

```cpp
          } else if (id(is_timer_active)) {
            id(control_leds_timer_ticking).execute();
          } else if (id(visualizer_enabled).state &&
                     id(external_media_player).state ==
                         media_player::MediaPlayerState::MEDIA_PLAYER_STATE_PLAYING) {
            auto &preset = id(visualizer_preset).state;
            auto call = id(voice_assistant_leds).make_call();
            if (preset == "Spectrum")
              call.set_effect("Visualizer Spectrum");
            else if (preset == "Pulse")
              call.set_effect("Visualizer Pulse");
            else if (preset == "VU Sweep")
              call.set_effect("Visualizer VU Sweep");
            else
              call.set_effect("Visualizer Waveform");
            call.perform();
          } else if (id(master_mute_switch).state) {
            id(control_leds_muted_or_silent).execute();
```

**Step 5: Update `config/satellite1.base.yaml`**

Add the new package after the `led_ring` line:

```yaml
  led_ring: !include common/led_ring.yaml
  visualizer: !include common/audio_visualizer.yaml
```

**Step 6: Compile**

```bash
source .venv/bin/activate
esphome compile config/satellite1.yaml 2>&1 | tail -30
```

Expected: `INFO Successfully compiled`. Common failures and fixes:

| Error | Fix |
|-------|-----|
| `'add_effects' is not a member of LightState` | Check ESPHome version; try `light_var.call().add_effects(...)` or look at how `led_ring.yaml` defines effects in `__init__.py` of the light component |
| `set_update_interval` not found | Replace with `set_update_interval_ms` or look at `esphome/components/light/light_effect.h` for the exact method name |
| `MediaPlayerState` not found | Include `esphome/components/media_player/media_player.h` or use the fully qualified enum |
| `audio_visualizer` component not found | Verify the directory structure: `esphome/components/audio_visualizer/speaker/__init__.py` must exist |

**Step 7: Run clang-format on all new files**

```bash
clang-format -i \
  esphome/components/audio_visualizer/speaker/audio_visualizer_speaker.h \
  esphome/components/audio_visualizer/speaker/audio_visualizer_speaker.cpp \
  esphome/components/audio_visualizer/speaker/visualizer_effect.h \
  esphome/components/audio_visualizer/speaker/visualizer_effect.cpp
```

**Step 8: Commit**

```bash
git add esphome/components/audio_visualizer/ \
        config/common/audio_visualizer.yaml \
        config/common/media_player.yaml \
        config/common/led_ring.yaml \
        config/satellite1.base.yaml
git commit -m "feat(audio_visualizer): wire visualizer into media pipeline and LED state machine"
```

**Step 9: Flash and manually test**

```bash
esphome upload config/satellite1.yaml
esphome logs config/satellite1.yaml
```

Test checklist:
1. Play music via Home Assistant → LED ring animates with **Spectrum** preset (24 freq bands)
2. Switch preset to **Pulse** → all LEDs pulse with the beat
3. Switch preset to **VU Sweep** → ring fills clockwise with loudness
4. Switch preset to **Waveform** → ripple orbits the ring
5. Pause music → LEDs return to idle dot-chase animation
6. Turn off "Visualizer Enabled" switch while playing → LEDs return to normal state
7. Re-enable while playing → visualizer resumes
8. Trigger wake word while visualizer is active → LED overrides to listening animation, returns to visualizer after

**Step 10: Tune normalization if needed**

If bands look too dim (never light up much), reduce the divisor in `analyze_window_()`:
```cpp
new_bands[b] = std::min(1.0f, (sum / count) / 10.0f);  // was 20.0f
```

If bands are always fully lit (everything at 1.0), increase it:
```cpp
new_bands[b] = std::min(1.0f, (sum / count) / 40.0f);
```

Similarly for `rms_` scale factor: `this->smoothed_rms_ * 5.0f` — adjust up/down to set sensitivity.

Commit after tuning:
```bash
git add esphome/components/audio_visualizer/speaker/audio_visualizer_speaker.cpp
git commit -m "fix(audio_visualizer): tune band normalization and RMS scale factor"
```
