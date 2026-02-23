# Directional Listening LED Animations — Design

**Date:** 2026-02-22
**Status:** Approved

## Overview

Add configurable listening LED animations to the Satellite1, including 4 new directional modes that use all 4 physical microphones to estimate the direction of the speaker and adjust LED brightness accordingly. The original rotating animation remains as the default option.

## Requirements

- Enable all 4 microphones (North, South, East, West) in the XMOS firmware
- Compute per-mic RMS energy on the XMOS and expose via SPI status register
- Estimate direction-of-arrival (DOA) on the ESP32 from mic energy levels
- Implement 5 selectable listening animation modes (1 original + 4 directional)
- Expose animation mode selection as a Home Assistant select entity
- Continuous real-time DOA tracking with smoothing

## Approach: Per-Mic Energy via SPI

The XMOS computes RMS energy per mic channel and exposes it through the existing SPI status register interface. The ESP32 reads the 4 energy values, estimates direction via weighted vector sum, and renders the selected LED effect. This approach was chosen over full phase-based DOA (too complex for XMOS changes) and raw 4-channel I2S (too risky to existing audio pipeline).

## Design

### 1. XMOS Firmware Changes

**Repo:** Fork of `Satellite1-XMOS`.

**Enable all 4 mics:** Change `MIC_MAPPING` from `"4, 5"` (East, West) to `"0, 1, 4, 5"` (North, South, East, West). Set `MIC_ARRAY_CONFIG_MIC_COUNT=4`.

**Per-mic RMS computation:** After PDM decimation but before the AEC pipeline, compute a running RMS energy value for each of the 4 mic channels. Use exponential moving average over ~32ms windows (~512 samples at 16kHz). Store as 4 × 16-bit unsigned integers (0–65535).

**SPI register exposure:** Add `DC_STATUS_MIC_ENERGY = 4` status register. Pack 4 × 16-bit RMS values into 2 × 32-bit reads:
- Word 0: `[North_16bit | South_16bit]`
- Word 1: `[East_16bit | West_16bit]`

**Existing pipeline unchanged:** 2-channel processed audio output to I2S stays as-is. RMS computation is a parallel tap on raw mic data.

### 2. ESP32 SPI Polling & Direction Estimation

**SPI polling:** Extend `Satellite1` component's SPI status register polling to read `DC_STATUS_MIC_ENERGY` at ~30Hz (every 33ms), aligned with LED update rate. Fits into existing `update()` loop.

**Direction estimation:** Convert 4 cardinal energy values to angle via 2D weighted vector sum:
```
x = East - West
y = North - South
angle = atan2(y, x) → 0–360°
```

**Smoothing:** Exponential moving average on (x, y) components (not angle directly, to avoid wrap-around discontinuities). Smoothing factor ~0.3, tunable.

**Confidence/threshold:** Total energy `magnitude = sqrt(x² + y²)`. Below a minimum threshold (silence/ambient), hold last known direction to prevent jitter.

**Output values:**
- `doa_angle` (0.0–360.0°) — smoothed direction of arrival
- `doa_confidence` (0.0–1.0) — normalized magnitude for brightness modulation

### 3. LED Animation Modes & HA Select Entity

**HA select entity** with 5 options (persists via `restore_value: true`):
- `Rotating` — current default, unchanged
- `Spotlight Beam`
- `Bright Arc`
- `Single Point`
- `Gradient Ring`

**Core mapping:** `doa_angle ÷ 15° = LED index` (360° ÷ 24 LEDs = 15° per LED).

**Mode brightness falloff curves:**

| Mode | Peak LEDs | Falloff |
|------|-----------|---------|
| Spotlight Beam | 3 (~45°) | Gaussian, sharp drop |
| Bright Arc | 6 (~90°) | Linear taper from center |
| Single Point | 1 + 2 neighbors | Step: 100% / 50% / 25% |
| Gradient Ring | All 24 | Cosine curve peaking at source |

**Brightness modulation:** `doa_confidence` scales overall brightness — loud/clear speech = bright LEDs, quiet speech = dimmer.

**Color:** Uses existing user-configured `led_ring` color (default cyan).

**Fallback:** `Rotating` mode or DOA data unavailable → original dual-point rotation plays unchanged.

**Scope:** Directional modes only active during `listening` voice assistant phase. All other phases unchanged.

### 4. Component Architecture & File Layout

**XMOS firmware fork:**
- `bsp_config/SATELLITE1/SATELLITE1.cmake` — `MIC_MAPPING` and `MIC_COUNT`
- `src/audio_pipeline/` — per-mic RMS computation tap
- `src/spi_slave/` — `DC_STATUS_MIC_ENERGY` register

**ESP32 firmware:**
- `esphome/components/satellite1/satellite1.h/.cpp` — SPI polling, DOA estimation
- `config/common/led_ring.yaml` — 4 new listening effect lambdas
- `config/common/home_assistant.yaml` — select entity

**Data flow:**
```
XMOS (4 mics → RMS per mic → SPI register)
    ↓ SPI poll @ 30Hz
Satellite1 component (read register → vector sum → smoothing)
    ↓ doa_angle, doa_confidence
LED ring scripts (select mode → render chosen effect)
    ↓
24 WS2812 LEDs
```

**Untouched:**
- Audio pipeline (I2S, AEC, voice assistant path)
- All other LED states (idle, thinking, replying, muted, error)
- Microphone component (continues reading 2-channel processed audio)
- Audio visualizer (media playback only)
