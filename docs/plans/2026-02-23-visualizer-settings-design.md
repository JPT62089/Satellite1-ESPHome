# Visualizer Adjustable Settings — Design

**Date:** 2026-02-23

## Overview

Add five runtime-adjustable settings to the LED audio visualizer effects: speed, intensity,
reverse, mirror, and start offset. All settings are exposed as Home Assistant entities
(number sliders and template switches), persist across reboots, and update the active effect
live when changed.

## HA Entities (`audio_visualizer.yaml`)

| Entity              | Type             | Range    | Default | Applies to                      |
|---------------------|------------------|----------|---------|----------------------------------|
| Visualizer Speed    | number (slider)  | 1–10     | 5       | All effects                      |
| Visualizer Intensity| number (slider)  | 0–200 %  | 100     | All effects                      |
| Visualizer Reverse  | switch (template)| on/off   | off     | All (Pulse: no-op)               |
| Visualizer Mirror   | switch (template)| on/off   | off     | All (Pulse: no-op)               |
| Visualizer Start    | number (slider)  | 0–23     | 0       | Spectrum, VU Sweep, Waveform     |

All entities are `entity_category: config` and use `restore_value: true` /
`restore_mode: RESTORE_DEFAULT_*`. Each has an `on_value` / `on_turn_on` / `on_turn_off`
that calls `script.execute: control_leds` so the active effect reacts immediately.

## C++ Changes (`visualizer_effect.h` / `.cpp`)

### Base class `VisualizerEffect`

Five new optional entity pointer members, all defaulting to `nullptr`:

```cpp
number::Number *speed_{nullptr};
number::Number *intensity_{nullptr};
switch_::Switch *reverse_{nullptr};
switch_::Switch *mirror_{nullptr};
number::Number *start_{nullptr};
```

Five public setters:

```cpp
void set_speed(number::Number *n);
void set_intensity(number::Number *n);
void set_reverse(switch_::Switch *s);
void set_mirror(switch_::Switch *s);
void set_start(number::Number *n);
```

Three safe-default helper accessors used by subclasses:

```cpp
float get_intensity_scale() const;  // returns intensity/100, default 1.0f
bool  get_reverse() const;          // default false
bool  get_mirror() const;           // default false
int   get_start_offset() const;     // returns (int)start->state, default 0
```

`should_update_()` derives the interval from the speed entity dynamically:
- `speed = 1` → 200 ms
- `speed = 10` → 16 ms
- Linear mapping; falls back to 33 ms if pointer is null.

### Per-effect behaviour

| Effect    | Speed       | Intensity                        | Reverse                          | Mirror                                    | Start                             |
|-----------|-------------|----------------------------------|----------------------------------|-------------------------------------------|-----------------------------------|
| Spectrum  | update rate | scale per-band brightness        | iterate LEDs N-1 → 0             | render N/2 bands, copy to second half     | LED index offset for all writes   |
| Pulse     | update rate | scale `smoothed_` + `beat_flash_`| no-op                            | no-op                                     | no-op                             |
| VU Sweep  | update rate | scale RMS before LED count calc  | fill from LED N-1 downward       | fill from both ends toward center         | LED index offset for all writes   |
| Waveform  | update rate | scale history brightness         | advance `head_` in reverse       | render orbit on both halves of ring       | LED index offset for all writes   |

For all LED index writes that support Start:
```cpp
it[(i + start_offset) % NUM_LEDS] = color;
```

For Mirror on Spectrum / VU Sweep / Waveform, after rendering the primary half:
```cpp
it[(start_offset + NUM_LEDS - 1 - i) % NUM_LEDS] = it[(start_offset + i) % NUM_LEDS];
```

## Codegen Changes (`speaker/__init__.py`)

`VISUALIZER_EFFECT_SCHEMA` gains five `cv.Optional` keys:

```python
cv.Optional(CONF_SPEED):     cv.use_id(number.Number),
cv.Optional(CONF_INTENSITY): cv.use_id(number.Number),
cv.Optional(CONF_REVERSE):   cv.use_id(switch_.Switch),
cv.Optional(CONF_MIRROR):    cv.use_id(switch_.Switch),
cv.Optional(CONF_START):     cv.use_id(number.Number),
```

`_visualizer_effect_to_code` calls `set_*` for whichever are present. The Pulse effect
codegen omits `set_reverse`, `set_mirror`, and `set_start`.

## YAML Effect Declarations (`led_ring.yaml`)

Each applicable effect declaration gains the five (or three for Pulse) entity references:

```yaml
- visualizer_spectrum:
    name: "Visualizer Spectrum"
    speed: viz_speed
    intensity: viz_intensity
    reverse: viz_reverse
    mirror: viz_mirror
    start: viz_start
```

Pulse omits `reverse`, `mirror`, and `start`.

## Files Touched

| File | Change |
|------|--------|
| `config/common/audio_visualizer.yaml` | Add 5 new entities |
| `config/common/led_ring.yaml` | Pass entity IDs into effect declarations |
| `esphome/components/audio_visualizer/speaker/visualizer_effect.h` | Add members, setters, helpers to base class |
| `esphome/components/audio_visualizer/speaker/visualizer_effect.cpp` | Implement updated apply() logic in all 4 effects |
| `esphome/components/audio_visualizer/speaker/__init__.py` | Extend schema and codegen |
