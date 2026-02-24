# Visualizer Adjustable Settings Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add five runtime-adjustable HA entity controls (speed, intensity, reverse, mirror, start) to the LED audio visualizer effects.

**Architecture:** Entity pointers are stored on the `VisualizerEffect` base class; each `apply()` reads them at call-time. Five template entities are declared in `audio_visualizer.yaml`; their IDs are wired into each effect declaration in `led_ring.yaml` via the `__init__.py` codegen.

**Tech Stack:** ESPHome C++ (ESP32-S3), ESPHome YAML config, Python codegen (`__init__.py`), clang-format v18.

---

## Reference: Files Touched

| File | Change |
|------|--------|
| `esphome/components/audio_visualizer/speaker/visualizer_effect.h` | Add entity pointer members, setters, inline helpers; update `should_update_()` |
| `esphome/components/audio_visualizer/speaker/visualizer_effect.cpp` | Update all four `apply()` implementations |
| `esphome/components/audio_visualizer/speaker/__init__.py` | Extend schema, remove hardcoded interval, wire setters |
| `config/common/audio_visualizer.yaml` | Add 5 new number/switch entities |
| `config/common/led_ring.yaml` | Pass entity IDs into each visualizer effect declaration |
| `tests/components/audio_visualizer/test_visualizer_settings.yaml` | New compile-test YAML |

---

### Task 1: C++ base class — entity pointers, setters, helpers, dynamic speed

**Files:**
- Modify: `esphome/components/audio_visualizer/speaker/visualizer_effect.h`

**Context:**
`VisualizerEffect` currently has `viz_`, `update_interval_` (hardcoded 33ms fallback), `last_run_`, and `should_update_()`. We add five optional entity pointer members and update `should_update_()` to derive interval from the speed entity at runtime.

The required ESPHome headers are:
- `esphome/components/number/number.h` → `number::Number`
- `esphome/components/switch/switch.h` → `switch_::Switch`

**Step 1: Add includes and members to the base class**

At the top of `visualizer_effect.h`, add after existing includes:
```cpp
#include "esphome/components/number/number.h"
#include "esphome/components/switch/switch.h"
```

In the `public:` section of `VisualizerEffect`, add setters:
```cpp
void set_speed(number::Number *n) { this->speed_ = n; }
void set_intensity(number::Number *n) { this->intensity_ = n; }
void set_reverse(switch_::Switch *s) { this->reverse_ = s; }
void set_mirror(switch_::Switch *s) { this->mirror_ = s; }
void set_start(number::Number *n) { this->start_ = n; }
```

Add inline helper accessors (also in `public:`):
```cpp
float get_intensity_scale() const {
  if (!this->intensity_)
    return 1.0f;
  return std::max(0.0f, this->intensity_->state / 100.0f);
}
bool get_reverse() const { return this->reverse_ && this->reverse_->state; }
bool get_mirror() const { return this->mirror_ && this->mirror_->state; }
int get_start_offset() const {
  return this->start_ ? (int) this->start_->state : 0;
}
```

Add five `protected:` members:
```cpp
number::Number *speed_{nullptr};
number::Number *intensity_{nullptr};
switch_::Switch *reverse_{nullptr};
switch_::Switch *mirror_{nullptr};
number::Number *start_{nullptr};
```

**Step 2: Update `should_update_()` to use the speed entity**

Replace the current `should_update_()` body with:
```cpp
bool should_update_() {
  uint32_t interval = this->update_interval_;  // fallback: 33ms
  if (this->speed_) {
    float s = std::max(1.0f, std::min(10.0f, this->speed_->state));
    // 1 → 200ms, 10 → 16ms, linear
    interval = (uint32_t) (200.0f - (s - 1.0f) * (184.0f / 9.0f));
  }
  uint32_t now = millis();
  if (now - this->last_run_ < interval)
    return false;
  this->last_run_ = now;
  return true;
}
```

**Step 3: Run clang-format and verify it's clean**

```bash
clang-format --dry-run esphome/components/audio_visualizer/speaker/visualizer_effect.h
```
Expected: no output (no formatting violations).

**Step 4: Commit**

```bash
git add esphome/components/audio_visualizer/speaker/visualizer_effect.h
git commit -m "feat(visualizer): add entity pointers and dynamic speed to base class"
```

---

### Task 2: Spectrum effect — intensity, reverse, mirror, start

**Files:**
- Modify: `esphome/components/audio_visualizer/speaker/visualizer_effect.cpp` (the `SpectrumRingEffect::apply` function)

**Context:**
Current logic: for each LED `i` (0..count-1), picks band `i`, hue sweeps blue→red, brightness = `sqrtf(bands[i])`. We parameterize all four settings.

**Step 1: Replace `SpectrumRingEffect::apply()`**

```cpp
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

    float hue = (count > 1) ? 0.67f * (1.0f - (float) band_i / (count - 1)) : 0.67f;
    float val = bands[band_i];
    float brightness = (val > 0.01f) ? std::min(1.0f, sqrtf(val) * scale) : 0.0f;
    it[(i + start) % count] = hsv_to_color(hue, 1.0f, brightness);
  }
  for (int i = count; i < it.size(); i++)
    it[i] = Color(0, 0, 0);
  it.schedule_show();
}
```

**Step 2: Run clang-format**

```bash
clang-format --dry-run esphome/components/audio_visualizer/speaker/visualizer_effect.cpp
```
Expected: no output.

**Step 3: Commit**

```bash
git add esphome/components/audio_visualizer/speaker/visualizer_effect.cpp
git commit -m "feat(visualizer): spectrum — intensity, reverse, mirror, start"
```

---

### Task 3: Pulse effect — intensity only

**Files:**
- Modify: `esphome/components/audio_visualizer/speaker/visualizer_effect.cpp` (the `PulseBeatEffect::apply` function)

**Context:**
Pulse lights all LEDs the same color — reverse/mirror/start have no meaningful effect. Only scale the brightness by `get_intensity_scale()`.

**Step 1: Update `PulseBeatEffect::apply()`**

Add `float scale = this->get_intensity_scale();` after the `consume_beat()` call, and apply it to the brightness calculation:
```cpp
float brightness = std::min(1.0f, (this->smoothed_ + this->beat_flash_ * 0.5f) * scale);
```

Full replacement:
```cpp
void PulseBeatEffect::apply(light::AddressableLight &it, const Color &current_color) {
  if (!this->viz_)
    return;
  if (!this->should_update_())
    return;
  float rms = this->viz_->get_rms();
  bool beat = this->viz_->consume_beat();
  float scale = this->get_intensity_scale();

  if (rms > this->smoothed_) {
    this->smoothed_ = 0.8f * rms + 0.2f * this->smoothed_;
  } else {
    this->smoothed_ = 0.1f * rms + 0.9f * this->smoothed_;
  }

  if (beat)
    this->beat_flash_ = 1.0f;
  this->beat_flash_ *= 0.75f;

  float brightness = std::min(1.0f, (this->smoothed_ + this->beat_flash_ * 0.5f) * scale);
  float sat = 1.0f - this->beat_flash_ * 0.7f;
  Color c = hsv_to_color(0.57f, sat, brightness);
  for (int i = 0; i < it.size(); i++)
    it[i] = c;
  it.schedule_show();
}
```

**Step 2: Run clang-format and commit**

```bash
clang-format --dry-run esphome/components/audio_visualizer/speaker/visualizer_effect.cpp
git add esphome/components/audio_visualizer/speaker/visualizer_effect.cpp
git commit -m "feat(visualizer): pulse — intensity scaling"
```

---

### Task 4: VU Sweep effect — intensity, reverse, mirror, start

**Files:**
- Modify: `esphome/components/audio_visualizer/speaker/visualizer_effect.cpp` (the `VUSweepEffect::apply` function)

**Context:**
VU Sweep fills `lit` LEDs clockwise from LED 0. Start shifts the origin. Reverse fills counterclockwise from the origin. Mirror fills symmetrically in both directions from the origin (each side gets `lit/2` LEDs). Mirror+Reverse fills from the antipode (origin + n/2) symmetrically instead.

The color gradient (green→yellow→red) follows the physical `pos` from the fill origin so the gradient always reads the same direction regardless of settings.

**Step 1: Replace `VUSweepEffect::apply()`**

```cpp
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

  int lit = (int) (rms * scale * n + 0.5f);
  lit = std::max(0, std::min(n, lit));

  for (int i = 0; i < n; i++) {
    // pos: clockwise distance from the start LED (0 = at start)
    int pos = (i - start + n) % n;

    bool on;
    if (mir) {
      int half_lit = lit / 2;
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
      float t = (n > 1) ? (float) pos / (n - 1) : 0.0f;
      Color c;
      if (t < 0.5f) {
        c = Color((uint8_t) (t * 2.0f * 255), 255, 0);
      } else {
        c = Color(255, (uint8_t) ((1.0f - (t - 0.5f) * 2.0f) * 255), 0);
      }
      it[i] = c;
    } else {
      it[i] = Color(0, 0, 0);
    }
  }
  it.schedule_show();
}
```

**Step 2: Run clang-format and commit**

```bash
clang-format --dry-run esphome/components/audio_visualizer/speaker/visualizer_effect.cpp
git add esphome/components/audio_visualizer/speaker/visualizer_effect.cpp
git commit -m "feat(visualizer): vu sweep — intensity, reverse, mirror, start"
```

---

### Task 5: Waveform Orbit effect — intensity, reverse, mirror, start

**Files:**
- Modify: `esphome/components/audio_visualizer/speaker/visualizer_effect.cpp` (the `WaveformOrbitEffect::apply` function)

**Context:**
Waveform orbits RMS history around the ring. Start shifts which physical LED receives newest energy. Reverse makes the orbit travel counterclockwise. Mirror renders the orbit simultaneously on both halves of the ring (newest energy at `start` and `start + count/2`).

**Step 1: Replace `WaveformOrbitEffect::apply()`**

```cpp
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
```

**Step 2: Run clang-format and commit**

```bash
clang-format --dry-run esphome/components/audio_visualizer/speaker/visualizer_effect.cpp
git add esphome/components/audio_visualizer/speaker/visualizer_effect.cpp
git commit -m "feat(visualizer): waveform — intensity, reverse, mirror, start"
```

---

### Task 6: Extend `__init__.py` schema and codegen

**Files:**
- Modify: `esphome/components/audio_visualizer/speaker/__init__.py`

**Context:**
Currently `_visualizer_effect_to_code` hardcodes `cg.add(var.set_update_interval(33))`. We remove that line (the base class default is already 33ms) and add optional schema keys + codegen for all 5 entities. All four effects share the same schema and codegen function.

**Step 1: Add imports**

After the existing imports, add:
```python
from esphome.components import number
from esphome.components import switch
```

**Step 2: Add local CONF constants after existing CONF_VISUALIZER**

```python
CONF_VIZ_SPEED = "speed"
CONF_VIZ_INTENSITY = "intensity"
CONF_VIZ_REVERSE = "reverse"
CONF_VIZ_MIRROR = "mirror"
CONF_VIZ_START = "start"
```

**Step 3: Update `VISUALIZER_EFFECT_SCHEMA`**

Replace the current schema dict with:
```python
VISUALIZER_EFFECT_SCHEMA = {
    cv.GenerateID(CONF_VISUALIZER): cv.use_id(AudioVisualizerSpeaker),
    cv.Optional(CONF_VIZ_SPEED): cv.use_id(number.Number),
    cv.Optional(CONF_VIZ_INTENSITY): cv.use_id(number.Number),
    cv.Optional(CONF_VIZ_REVERSE): cv.use_id(switch.Switch),
    cv.Optional(CONF_VIZ_MIRROR): cv.use_id(switch.Switch),
    cv.Optional(CONF_VIZ_START): cv.use_id(number.Number),
}
```

**Step 4: Update `_visualizer_effect_to_code`**

Replace the current function with:
```python
async def _visualizer_effect_to_code(config, effect_id):
    """Shared codegen for all visualizer effects."""
    var = cg.new_Pvariable(effect_id, config[CONF_NAME])
    viz = await cg.get_variable(config[CONF_VISUALIZER])
    cg.add(var.set_visualizer(viz))
    # Note: set_update_interval() is intentionally omitted — speed entity drives
    # the interval dynamically; base class default (33ms) is the fallback.
    if CONF_VIZ_SPEED in config:
        speed = await cg.get_variable(config[CONF_VIZ_SPEED])
        cg.add(var.set_speed(speed))
    if CONF_VIZ_INTENSITY in config:
        intensity = await cg.get_variable(config[CONF_VIZ_INTENSITY])
        cg.add(var.set_intensity(intensity))
    if CONF_VIZ_REVERSE in config:
        rev = await cg.get_variable(config[CONF_VIZ_REVERSE])
        cg.add(var.set_reverse(rev))
    if CONF_VIZ_MIRROR in config:
        mir = await cg.get_variable(config[CONF_VIZ_MIRROR])
        cg.add(var.set_mirror(mir))
    if CONF_VIZ_START in config:
        st = await cg.get_variable(config[CONF_VIZ_START])
        cg.add(var.set_start(st))
    return var
```

All four `visualizer_*_to_code` functions already delegate to `_visualizer_effect_to_code` and need no changes.

**Step 5: Commit**

```bash
git add esphome/components/audio_visualizer/speaker/__init__.py
git commit -m "feat(visualizer): extend codegen schema for speed/intensity/reverse/mirror/start"
```

---

### Task 7: Add 5 HA entities to `audio_visualizer.yaml`

**Files:**
- Modify: `config/common/audio_visualizer.yaml`

**Context:**
Append a `number:` block and expand the `switch:` block. All entities are `entity_category: config`. Numbers use `restore_value: true`; switches use `restore_mode: RESTORE_DEFAULT_*`. Each fires `control_leds` on change.

**Step 1: Add number entities**

Append to `config/common/audio_visualizer.yaml`:
```yaml
number:
  - platform: template
    name: "Visualizer Speed"
    id: viz_speed
    icon: "mdi:speedometer"
    entity_category: config
    min_value: 1
    max_value: 10
    step: 1
    initial_value: 5
    restore_value: true
    optimistic: true
    on_value:
      - if:
          condition:
            switch.is_on: visualizer_enabled
          then:
            - script.execute: control_leds

  - platform: template
    name: "Visualizer Intensity"
    id: viz_intensity
    icon: "mdi:brightness-6"
    entity_category: config
    unit_of_measurement: "%"
    min_value: 0
    max_value: 200
    step: 5
    initial_value: 100
    restore_value: true
    optimistic: true
    on_value:
      - if:
          condition:
            switch.is_on: visualizer_enabled
          then:
            - script.execute: control_leds

  - platform: template
    name: "Visualizer Start"
    id: viz_start
    icon: "mdi:map-marker-circle"
    entity_category: config
    min_value: 0
    max_value: 23
    step: 1
    initial_value: 0
    restore_value: true
    optimistic: true
    on_value:
      - if:
          condition:
            switch.is_on: visualizer_enabled
          then:
            - script.execute: control_leds
```

**Step 2: Add reverse and mirror switches**

Append two more entries to the existing `switch:` block in the same file:
```yaml
  - platform: template
    name: "Visualizer Reverse"
    id: viz_reverse
    icon: "mdi:swap-horizontal"
    entity_category: config
    restore_mode: RESTORE_DEFAULT_OFF
    optimistic: true
    on_turn_on:
      - if:
          condition:
            switch.is_on: visualizer_enabled
          then:
            - script.execute: control_leds
    on_turn_off:
      - if:
          condition:
            switch.is_on: visualizer_enabled
          then:
            - script.execute: control_leds

  - platform: template
    name: "Visualizer Mirror"
    id: viz_mirror
    icon: "mdi:mirror"
    entity_category: config
    restore_mode: RESTORE_DEFAULT_OFF
    optimistic: true
    on_turn_on:
      - if:
          condition:
            switch.is_on: visualizer_enabled
          then:
            - script.execute: control_leds
    on_turn_off:
      - if:
          condition:
            switch.is_on: visualizer_enabled
          then:
            - script.execute: control_leds
```

**Step 3: Run yamllint**

```bash
yamllint config/common/audio_visualizer.yaml
```
Expected: no errors.

**Step 4: Commit**

```bash
git add config/common/audio_visualizer.yaml
git commit -m "feat(visualizer): add speed, intensity, reverse, mirror, start HA entities"
```

---

### Task 8: Wire entity IDs into effect declarations in `led_ring.yaml`

**Files:**
- Modify: `config/common/led_ring.yaml`

**Context:**
The four visualizer effects are currently declared as bare name-only entries (lines ~505–512). Replace each with the parameterized form. All five entity keys are passed to all effects; Pulse silently ignores reverse/mirror/start in its `apply()`.

**Step 1: Replace the four visualizer effect declarations**

Find:
```yaml
      - visualizer_spectrum:
          name: "Visualizer Spectrum"
      - visualizer_pulse:
          name: "Visualizer Pulse"
      - visualizer_vu_sweep:
          name: "Visualizer VU Sweep"
      - visualizer_waveform:
          name: "Visualizer Waveform"
```

Replace with:
```yaml
      - visualizer_spectrum:
          name: "Visualizer Spectrum"
          speed: viz_speed
          intensity: viz_intensity
          reverse: viz_reverse
          mirror: viz_mirror
          start: viz_start
      - visualizer_pulse:
          name: "Visualizer Pulse"
          speed: viz_speed
          intensity: viz_intensity
          reverse: viz_reverse
          mirror: viz_mirror
          start: viz_start
      - visualizer_vu_sweep:
          name: "Visualizer VU Sweep"
          speed: viz_speed
          intensity: viz_intensity
          reverse: viz_reverse
          mirror: viz_mirror
          start: viz_start
      - visualizer_waveform:
          name: "Visualizer Waveform"
          speed: viz_speed
          intensity: viz_intensity
          reverse: viz_reverse
          mirror: viz_mirror
          start: viz_start
```

**Step 2: Run yamllint**

```bash
yamllint config/common/led_ring.yaml
```
Expected: no errors.

**Step 3: Commit**

```bash
git add config/common/led_ring.yaml
git commit -m "feat(visualizer): wire speed/intensity/reverse/mirror/start into effect declarations"
```

---

### Task 9: Compile test — create test YAML and verify full build

**Files:**
- Create: `tests/components/audio_visualizer/test_visualizer_settings.yaml`

**Context:**
ESPHome "tests" are YAML configs that must compile without error. Model the structure after `tests/components/sendspin/test_sendspin.yaml`. The test must instantiate the audio_visualizer speaker, declare all 5 setting entities, and reference them in a visualizer effect — all on a standalone config that doesn't depend on `satellite1.yaml`.

**Step 1: Create the compile test YAML**

```yaml
substitutions:
  friendly_name: "Satellite1 Visualizer Settings Test"
  node_name: sat1-visualizer-test

esphome:
  name: ${node_name}
  friendly_name: ${friendly_name}
  min_version: 2026.1.0

esp32:
  board: esp32-s3-devkitc-1
  variant: ESP32S3
  flash_size: 16MB
  framework:
    type: esp-idf

psram:
  mode: octal
  speed: 80MHz

wifi:
  ssid: "test-ssid"
  password: "test-password"

logger:
  level: DEBUG

api:

external_components:
  - source:
      type: local
      path: ../../../esphome/components
    components:
      - audio
      - audio_visualizer
      - i2s_audio
      - mixer
      - resampler
      - speaker

i2s_audio:
  - id: i2s_shared
    i2s_lrclk_pin: GPIO3
    i2s_bclk_pin: GPIO2
    i2s_mclk_pin: GPIO16

speaker:
  - platform: i2s_audio
    id: i2s_audio_speaker
    sample_rate: 48000
    i2s_dout_pin: GPIO9
    bits_per_sample: 32bit
    i2s_audio_id: i2s_shared
    dac_type: external
    channel: stereo
    timeout: never

  - platform: mixer
    id: mixing_speaker
    output_speaker: i2s_audio_speaker
    num_channels: 2
    source_speakers:
      - id: media_mixing_input
        timeout: never

  - platform: audio_visualizer
    id: audio_viz
    output_speaker: media_mixing_input
    bits_per_sample: 16
    num_channels: 2
    sample_rate: 48000

light:
  - platform: esp32_rmt_led_strip
    id: hw_leds
    pin: GPIO21
    num_leds: 24
    rgb_order: GRB
    chipset: WS2812
    effects:
      - visualizer_spectrum:
          name: "Visualizer Spectrum"
          speed: viz_speed
          intensity: viz_intensity
          reverse: viz_reverse
          mirror: viz_mirror
          start: viz_start
      - visualizer_pulse:
          name: "Visualizer Pulse"
          speed: viz_speed
          intensity: viz_intensity
          reverse: viz_reverse
          mirror: viz_mirror
          start: viz_start
      - visualizer_vu_sweep:
          name: "Visualizer VU Sweep"
          speed: viz_speed
          intensity: viz_intensity
          reverse: viz_reverse
          mirror: viz_mirror
          start: viz_start
      - visualizer_waveform:
          name: "Visualizer Waveform"
          speed: viz_speed
          intensity: viz_intensity
          reverse: viz_reverse
          mirror: viz_mirror
          start: viz_start

number:
  - platform: template
    name: "Visualizer Speed"
    id: viz_speed
    min_value: 1
    max_value: 10
    step: 1
    initial_value: 5
    restore_value: true
    optimistic: true

  - platform: template
    name: "Visualizer Intensity"
    id: viz_intensity
    min_value: 0
    max_value: 200
    step: 5
    initial_value: 100
    restore_value: true
    optimistic: true

  - platform: template
    name: "Visualizer Start"
    id: viz_start
    min_value: 0
    max_value: 23
    step: 1
    initial_value: 0
    restore_value: true
    optimistic: true

switch:
  - platform: template
    name: "Visualizer Reverse"
    id: viz_reverse
    restore_mode: RESTORE_DEFAULT_OFF
    optimistic: true

  - platform: template
    name: "Visualizer Mirror"
    id: viz_mirror
    restore_mode: RESTORE_DEFAULT_OFF
    optimistic: true
```

**Step 2: Run yamllint on the test file**

```bash
yamllint tests/components/audio_visualizer/test_visualizer_settings.yaml
```
Expected: no errors.

**Step 3: Compile the test YAML**

```bash
source .venv/bin/activate
esphome compile tests/components/audio_visualizer/test_visualizer_settings.yaml
```
Expected: `INFO Compilation successful` (or equivalent success message). Fix any compile errors before proceeding.

**Step 4: Compile the main satellite1 config**

```bash
esphome compile config/satellite1.yaml
```
Expected: success.

**Step 5: Commit**

```bash
git add tests/components/audio_visualizer/test_visualizer_settings.yaml
git commit -m "test(visualizer): add compile test for adjustable settings"
```

---

## Done

All five adjustable settings (speed, intensity, reverse, mirror, start) are now wired end-to-end: HA entities → YAML effect declarations → C++ base class → each effect's `apply()`. The compile test confirms the full stack builds cleanly.
