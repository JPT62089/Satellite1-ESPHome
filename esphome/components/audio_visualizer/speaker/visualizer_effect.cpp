#ifdef USE_ESP32

#include "visualizer_effect.h"

#include <algorithm>
#include <cmath>

namespace esphome {
namespace audio_visualizer {

// --- Preset 1: Spectrum Ring ---
void SpectrumRingEffect::apply(light::AddressableLight &it, const Color &current_color) {
  if (!this->viz_)
    return;
  if (!this->should_update_())
    return;
  float bands[NUM_BANDS] = {};
  this->viz_->get_bands(bands);

  int count = std::min((int) it.size(), (int) NUM_BANDS);
  float scale = this->get_intensity_scale();
  bool rev = this->get_reverse();
  bool mir = this->get_mirror();
  int start = this->get_start_offset();
  int half = count / 2;

  for (int i = 0; i < count; i++) {
    int band_i;
    if (mir) {
      // Symmetric: LEDs 0..half-1 and count-1..half use the same N/2 bands
      int mirror_i = (i < half) ? i : (count - 1 - i);
      band_i = rev ? (half - 1 - mirror_i) : mirror_i;
    } else {
      band_i = rev ? (count - 1 - i) : i;
    }
    band_i = std::max(0, std::min(count - 1, band_i));

    // Hue sweeps from 0.67 (blue) at bass to 0.0 (red) at treble
    float hue = (count > 1) ? 0.67f * (1.0f - (float) band_i / (count - 1)) : 0.67f;
    float val = bands[band_i];
    // Sqrt compression keeps quieter high-frequency bands visible; scale applies intensity
    float brightness = (val > 0.01f) ? std::min(1.0f, sqrtf(val) * scale) : 0.0f;
    it[(i + start) % count] = hsv_to_color(hue, 1.0f, brightness);
  }
  for (int i = count; i < it.size(); i++)
    it[i] = Color(0, 0, 0);
  it.schedule_show();
}

// --- Preset 2: Pulse / Beat ---
void PulseBeatEffect::apply(light::AddressableLight &it, const Color &current_color) {
  if (!this->viz_)
    return;
  if (!this->should_update_())
    return;
  float rms = this->viz_->get_rms();
  bool beat = this->viz_->consume_beat();
  float scale = this->get_intensity_scale();

  // Asymmetric envelope: fast attack, slow decay
  if (rms > this->smoothed_) {
    this->smoothed_ = 0.8f * rms + 0.2f * this->smoothed_;
  } else {
    this->smoothed_ = 0.1f * rms + 0.9f * this->smoothed_;
  }

  if (beat)
    this->beat_flash_ = 1.0f;
  this->beat_flash_ *= 0.75f;  // decay flash over several frames

  float brightness = std::min(1.0f, (this->smoothed_ + this->beat_flash_ * 0.5f) * scale);
  // Desaturate toward white on beat flash
  float sat = 1.0f - this->beat_flash_ * 0.7f;
  Color c = hsv_to_color(0.57f, sat, brightness);  // sky-blue base
  for (int i = 0; i < it.size(); i++)
    it[i] = c;
  it.schedule_show();
}

// --- Preset 3: VU Sweep ---
void VUSweepEffect::apply(light::AddressableLight &it, const Color &current_color) {
  if (!this->viz_)
    return;
  if (!this->should_update_())
    return;
  float rms = this->viz_->get_rms();
  float scale = this->get_intensity_scale();
  bool rev = this->get_reverse();
  bool mir = this->get_mirror();
  int start = this->get_start_offset();
  int n = it.size();

  // Scale RMS by intensity; lit count is how many LEDs to fill
  int lit = (int) (rms * scale * n + 0.5f);
  lit = std::max(0, std::min(n, lit));

  for (int i = 0; i < n; i++) {
    // pos: clockwise distance from the start LED (0 = at start)
    int pos = (i - start + n) % n;

    bool on;
    if (mir) {
      int half_lit = (lit + 1) / 2;
      if (rev) {
        // Fill from antipode (start + n/2) symmetrically in both directions
        int apos = (pos + n / 2) % n;  // distance from antipode
        on = (apos < half_lit) || (apos >= n - half_lit);
      } else {
        // Fill from start symmetrically clockwise and counterclockwise
        on = (pos < half_lit) || (pos >= n - half_lit);
      }
    } else {
      // Fill as a single arc
      on = rev ? (pos >= n - lit) : (pos < lit);
    }

    if (on) {
      // Gradient anchored to pos=0 (start), not to the fill direction.
      // In reverse mode the lit arc occupies high pos values, so the
      // perceived color-to-level mapping is warm near start, cool at far edge.
      float t = (n > 1) ? (float) pos / (n - 1) : 0.0f;
      Color c;
      if (t < 0.5f) {
        // green to yellow
        c = Color((uint8_t) (t * 2.0f * 255), 255, 0);
      } else {
        // yellow to red
        c = Color(255, (uint8_t) ((1.0f - (t - 0.5f) * 2.0f) * 255), 0);
      }
      it[i] = c;
    } else {
      it[i] = Color(0, 0, 0);
    }
  }
  it.schedule_show();
}

// --- Preset 4: Waveform Orbit ---
void WaveformOrbitEffect::apply(light::AddressableLight &it, const Color &current_color) {
  if (!this->viz_)
    return;
  if (!this->should_update_())
    return;
  float rms = this->viz_->get_rms();

  // Always push new sample at head and advance forward
  this->history_[this->head_] = rms;
  this->head_ = (this->head_ + 1) % NUM_BANDS;

  int count = std::min((int) it.size(), (int) NUM_BANDS);
  float scale = this->get_intensity_scale();
  bool rev = this->get_reverse();
  bool mir = this->get_mirror();
  int start = this->get_start_offset();

  if (mir) {
    // Show orbit on both halves simultaneously
    // Note: if count is odd, the physical midpoint LED retains its previous value.
    // The hardware ring has 24 LEDs (even), so this never occurs in practice.
    int half = count / 2;
    for (int i = 0; i < half; i++) {
      uint32_t idx = (this->head_ + NUM_BANDS - 1 - i) % NUM_BANDS;
      float brightness = std::min(1.0f, this->history_[idx] * scale);
      Color c = hsv_to_color(0.57f, 1.0f, brightness);
      if (rev) {
        // Newest energy at antipode, orbits toward start from both sides
        it[(start + count / 2 + i) % count] = c;
        it[(start + count / 2 - 1 - i + count) % count] = c;
      } else {
        // Newest energy at start, orbits away in both directions
        it[(start + i) % count] = c;
        it[(start + count - 1 - i) % count] = c;
      }
    }
  } else {
    for (int i = 0; i < count; i++) {
      uint32_t idx = (this->head_ + NUM_BANDS - 1 - i) % NUM_BANDS;
      float brightness = std::min(1.0f, this->history_[idx] * scale);
      // rev: newest at start, trails counterclockwise; else trails clockwise
      int led = rev ? (start - i + count) % count : (start + i) % count;
      it[led] = hsv_to_color(0.57f, 1.0f, brightness);
    }
  }
  for (int i = count; i < it.size(); i++)
    it[i] = Color(0, 0, 0);
  it.schedule_show();
}

}  // namespace audio_visualizer
}  // namespace esphome

#endif  // USE_ESP32
