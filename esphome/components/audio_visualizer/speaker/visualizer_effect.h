#pragma once
#ifdef USE_ESP32

#include "audio_visualizer_speaker.h"
#include "esphome/components/light/addressable_light_effect.h"

#include <cmath>

namespace esphome {
namespace audio_visualizer {

// HSV to ESPHome Color helper (h, s, v all in [0, 1])
inline Color hsv_to_color(float h, float s, float v) {
  h = fmodf(h, 1.0f);
  if (h < 0.0f)
    h += 1.0f;
  float r = 0, g = 0, b = 0;
  int i = (int) (h * 6);
  float f = h * 6.0f - i;
  float p = v * (1.0f - s);
  float q = v * (1.0f - f * s);
  float t = v * (1.0f - (1.0f - f) * s);
  switch (i % 6) {
    case 0:
      r = v;
      g = t;
      b = p;
      break;
    case 1:
      r = q;
      g = v;
      b = p;
      break;
    case 2:
      r = p;
      g = v;
      b = t;
      break;
    case 3:
      r = p;
      g = q;
      b = v;
      break;
    case 4:
      r = t;
      g = p;
      b = v;
      break;
    case 5:
      r = v;
      g = p;
      b = q;
      break;
  }
  return Color((uint8_t) (r * 255), (uint8_t) (g * 255), (uint8_t) (b * 255));
}

// Base class -- all presets hold a reference to the visualizer speaker
class VisualizerEffect : public light::AddressableLightEffect {
 public:
  explicit VisualizerEffect(const char *name) : AddressableLightEffect(name) {}
  void set_visualizer(AudioVisualizerSpeaker *viz) { this->viz_ = viz; }
  void set_update_interval(uint32_t ms) { this->update_interval_ = ms; }

 protected:
  /// Returns true if enough time has elapsed since last_run_. Call at the start of apply().
  bool should_update_() {
    uint32_t now = millis();
    if (now - this->last_run_ < this->update_interval_)
      return false;
    this->last_run_ = now;
    return true;
  }

  AudioVisualizerSpeaker *viz_{nullptr};
  uint32_t update_interval_{33};
  uint32_t last_run_{0};
};

// Preset 1: Spectrum Ring
// Each LED = one frequency band; color gradient blue (bass) to red (treble)
class SpectrumRingEffect : public VisualizerEffect {
 public:
  explicit SpectrumRingEffect(const char *name) : VisualizerEffect(name) {}
  void apply(light::AddressableLight &it, const Color &current_color) override;
};

// Preset 2: Pulse / Beat
// All LEDs pulse with RMS; flashes white on beat
class PulseBeatEffect : public VisualizerEffect {
 public:
  explicit PulseBeatEffect(const char *name) : VisualizerEffect(name) {}
  void apply(light::AddressableLight &it, const Color &current_color) override;

 protected:
  float smoothed_{0.0f};
  float beat_flash_{0.0f};
};

// Preset 3: VU Sweep
// LEDs fill clockwise with RMS; green to yellow to red gradient
class VUSweepEffect : public VisualizerEffect {
 public:
  explicit VUSweepEffect(const char *name) : VisualizerEffect(name) {}
  void apply(light::AddressableLight &it, const Color &current_color) override;
};

// Preset 4: Waveform Orbit
// RMS history orbits the ring: new energy enters at LED 0 and chases around
class WaveformOrbitEffect : public VisualizerEffect {
 public:
  explicit WaveformOrbitEffect(const char *name) : VisualizerEffect(name) {}
  void apply(light::AddressableLight &it, const Color &current_color) override;

 protected:
  float history_[NUM_BANDS]{};
  uint32_t head_{0};
};

}  // namespace audio_visualizer
}  // namespace esphome

#endif  // USE_ESP32
