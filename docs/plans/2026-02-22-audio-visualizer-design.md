# Audio Visualizer Design

**Date**: 2026-02-22
**Branch**: develop

## Overview

Add a Winamp-style LED ring visualizer that reacts to music playback on the Satellite1 device. When media is playing and the visualizer is enabled, the 24 WS2812 LEDs animate in sync with the audio. Four presets are available. The feature integrates cleanly with the existing LED state machine priority system and has zero audible impact on playback.

## Goals

- Audio-reactive LED animations driven by the PCM playback stream
- Four visual presets: Spectrum, Pulse, VU Sweep, Waveform Orbit
- Enable/disable switch exposed to Home Assistant
- Preset selector exposed to Home Assistant
- Auto-activates when media is playing + switch is on; yields back to normal LED states when music stops
- No new FreeRTOS tasks; no external library dependencies

## Non-Goals

- Microphone-based audio analysis
- More than four presets in the initial implementation
- Per-preset color customization in the initial implementation (base colors are hardcoded per preset)

---

## Architecture

### New Component: `audio_visualizer`

**Location**: `esphome/components/audio_visualizer/`

| File | Purpose |
|------|---------|
| `__init__.py` | ESPHome config schema; codegen to wire the speaker chain and inject effects into the target light |
| `audio_visualizer_speaker.h/.cpp` | Pass-through `speaker::Speaker` — accumulates PCM, runs analysis, publishes results |
| `visualizer_effect.h/.cpp` | Base `AddressableLightEffect` subclass + 4 concrete preset classes |

### Speaker Chain Integration

The `AudioVisualizerSpeaker` sits between `media_resampling_speaker` and `media_mixing_input` in the existing media player audio chain:

```
AudioPipeline (decoder)
    ↓
media_resampling_speaker (48kHz, 16-bit)
    ↓
[NEW] AudioVisualizerSpeaker  ← PCM tap point
    ↓
media_mixing_input (SourceSpeaker)
    ↓
mixing_speaker (MixerSpeaker)
    ↓
i2s_audio_speaker → DAC
```

All PCM data is forwarded unchanged to `output_speaker_`. The visualizer is transparent to the audio path.

---

## Signal Processing Pipeline

### Analysis Window

- **Window size**: 512 samples
- **Overlap**: 50% (analysis triggered every 256 new samples)
- **Rate**: ~94 analysis frames/second at 48kHz
- **LED update interval**: 33ms (~30fps)
- **Stereo handling**: channels averaged to mono before analysis

### Per-Window Steps

1. **RMS amplitude** — computed from raw samples before windowing, for accurate loudness representation
2. **Hann window** — applied to the 512 samples to reduce spectral leakage
3. **512-point FFT** — self-contained iterative Cooley-Tukey implementation (~100 lines C++, no external dependencies). Runs in ~50µs on ESP32-S3 @ 240MHz.
4. **24 logarithmic frequency bands** — FFT magnitude bins mapped logarithmically to 24 bands (~60Hz–16kHz). Logarithmic mapping matches human hearing and prevents bass domination.
5. **Exponential smoothing** — applied to both RMS and band values:
   - Bands: α = 0.3 (slower, fluid feel)
   - RMS: α = 0.6 (faster, more responsive)
   - Formula: `output = α·new + (1-α)·prev`
6. **Beat detection** — when bass band energy exceeds 1.5× its rolling average, `beat_` flag is set for one analysis frame

### Shared State

Written by the audio task, read by the light effect task:

```cpp
std::atomic<float> rms_;    // 0.0–1.0 overall loudness
float bands_[24];            // per-band energy, 0.0–1.0 (mutex-protected)
std::atomic<bool> beat_;     // true for one frame on beat detection
```

---

## LED Preset Designs

All presets update at 30fps (33ms interval) via `AddressableLightEffect` subclasses registered on `voice_assistant_leds`.

### Preset 1 — Spectrum Ring

Each of the 24 LEDs maps to one frequency band (LED 0 = bass, LED 23 = treble). Brightness proportional to band energy. Fixed color gradient: blue (bass) → cyan → green → yellow → red (treble). LEDs dim via exponential decay so they don't snap off abruptly.

### Preset 2 — Pulse / Beat

All 24 LEDs act as one unit. Brightness tracks `rms_` with asymmetric envelope: fast attack (α = 0.8) and slow decay (α = 0.1). On `beat_` detection, brightness slams to 1.0 and color briefly shifts toward white before returning to the base hue.

### Preset 3 — VU Sweep

`rms_` maps to LED fill count (0.0 = 0 LEDs, 1.0 = 24 LEDs). LEDs fill clockwise from LED 0. Color gradient by fill level: green (0–50%) → yellow (50–75%) → red (75–100%). Unlit LEDs are fully off.

### Preset 4 — Waveform Orbit

A circular history buffer stores the last 24 RMS snapshots. Each LED's brightness equals the RMS value from N frames ago, where N is the LED's index. New energy enters at LED 0 and chases around the ring, creating a ripple effect that orbits the ring in sync with the beat.

---

## Configuration

### New file: `config/common/audio_visualizer.yaml`

```yaml
audio_visualizer:
  id: audio_viz
  speaker_input: media_resampling_speaker
  output_speaker: media_mixing_input
  light: voice_assistant_leds

switch:
  - platform: template
    name: "Visualizer Enabled"
    id: visualizer_enabled
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
    options: ["Spectrum", "Pulse", "VU Sweep", "Waveform"]
    initial_option: "Spectrum"
    optimistic: true
    on_value:
      - script.execute: control_leds
```

### `control_leds` Priority Block (added to `led_ring.yaml`)

Slotted below voice assistant states, above media player mute/volume display:

```yaml
- if:
    condition:
      and:
        - switch.is_on: visualizer_enabled
        - media_player.is_playing: external_media_player
    then:
      - lambda: |
          auto &preset = id(visualizer_preset).state;
          auto call = id(voice_assistant_leds).make_call();
          if (preset == "Spectrum")       call.set_effect("Visualizer Spectrum");
          else if (preset == "Pulse")     call.set_effect("Visualizer Pulse");
          else if (preset == "VU Sweep")  call.set_effect("Visualizer VU Sweep");
          else                            call.set_effect("Visualizer Waveform");
          call.perform();
      - return
```

When music stops, `control_leds` re-runs (triggered by media player state change) and falls through to the normal LED state machine — seamless handoff.

---

## Files Changed

| File | Change |
|------|--------|
| `esphome/components/audio_visualizer/__init__.py` | New |
| `esphome/components/audio_visualizer/audio_visualizer_speaker.h` | New |
| `esphome/components/audio_visualizer/audio_visualizer_speaker.cpp` | New |
| `esphome/components/audio_visualizer/visualizer_effect.h` | New |
| `esphome/components/audio_visualizer/visualizer_effect.cpp` | New |
| `config/common/audio_visualizer.yaml` | New |
| `config/common/led_ring.yaml` | Add 4 effects to `voice_assistant_leds`; add visualizer priority block to `control_leds` |
| `config/common/media_player.yaml` | Rewire `media_resampling_speaker` output through `audio_viz` |
| `config/satellite1.base.yaml` | Include `audio_visualizer.yaml` package |
