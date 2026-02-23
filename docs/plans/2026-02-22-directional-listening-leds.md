# Directional Listening LED Animations — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add 5 selectable listening LED animation modes — the original rotating pattern plus 4 directional modes that use per-mic energy from all 4 physical microphones to indicate the speaker's direction.

**Architecture:** The XMOS firmware (forked repo) enables all 4 PDM mics, computes per-mic RMS energy every audio frame, and exposes 4 × 8-bit energy values via existing SPI status registers. The ESP32 reads these values, computes a smoothed DOA angle + confidence via 2D vector sum, and renders the selected LED effect. A Home Assistant select entity lets users choose the animation mode.

**Tech Stack:** XMOS xcore.ai (C, FreeRTOS), ESP32-S3 (ESPHome C++/YAML), SPI device control protocol

**Repos:**
- ESP32: `/home/jeremy/GitHub/FutureProofHomes/Satellite1-ESPHome` (this repo)
- XMOS: `/home/jeremy/GitHub/FutureProofHomes/Satellite1-XMOS` (fork)

---

## Task 1: ESP32 — Add DOA globals, select entity, and animation mode infrastructure

**Files:**
- Modify: `config/common/led_ring.yaml:1-6` (globals section)
- Modify: `config/common/home_assistant.yaml:63` (after master_mute_switch)

### Step 1: Add DOA globals and animation mode global to led_ring.yaml

Add these globals after the existing `jack_unplugged_recently` global (after line 16):

```yaml
  # Direction-of-arrival angle from mic energy analysis (0.0 - 360.0 degrees, 0=North)
  - id: doa_angle
    type: float
    restore_value: no
    initial_value: '0.0'
  # Direction-of-arrival confidence (0.0 - 1.0, normalized magnitude)
  - id: doa_confidence
    type: float
    restore_value: no
    initial_value: '0.0'
```

### Step 2: Add listening animation select entity to home_assistant.yaml

Add after the `master_mute_switch` block (after line 62):

```yaml
select:
  - platform: template
    id: listening_animation_mode
    name: "Listening Animation"
    icon: "mdi:led-strip-variant"
    entity_category: config
    optimistic: true
    restore_value: true
    initial_option: "Rotating"
    options:
      - "Rotating"
      - "Spotlight Beam"
      - "Bright Arc"
      - "Single Point"
      - "Gradient Ring"
    set_action:
      - lambda: |-
          id(listening_animation_mode).publish_state(x);
```

### Step 3: Compile to verify

Run: `esphome compile config/satellite1.yaml`
Expected: Successful compilation with new globals and select entity.

### Step 4: Commit

```bash
git add config/common/led_ring.yaml config/common/home_assistant.yaml
git commit -m "feat(leds): add DOA globals and listening animation select entity"
```

---

## Task 2: ESP32 — Add 4 directional LED effects and wire animation mode selection

**Files:**
- Modify: `config/common/led_ring.yaml:83-107` (effects section, after "Listening For Command")
- Modify: `config/common/led_ring.yaml:653-658` (listening script)

### Step 1: Add 4 directional effect lambdas to voice_assistant_leds

Add these 4 effects after the existing "Listening For Command" effect (after line 107 in led_ring.yaml), inside the `effects:` list of `voice_assistant_leds`:

```yaml
      - addressable_lambda:
          name: "Listening Spotlight Beam"
          update_interval: 33ms
          lambda: |-
            auto light_color = id(led_ring).current_values;
            Color color(light_color.get_red() * 255, light_color.get_green() * 255,
                  light_color.get_blue() * 255);
            float angle = id(doa_angle);
            float conf = id(doa_confidence);
            int center = ((int)(angle / 15.0f)) % ${led_ring_count};
            for (int i = 0; i < ${led_ring_count}; i++) {
              // Signed shortest distance around ring
              int diff = i - center;
              if (diff > ${led_ring_count} / 2) diff -= ${led_ring_count};
              if (diff < -${led_ring_count} / 2) diff += ${led_ring_count};
              int abs_diff = diff < 0 ? -diff : diff;
              // Gaussian falloff: sigma ~1.2 LEDs (3 LEDs wide at half-max)
              float brightness = expf(-0.5f * (abs_diff * abs_diff) / 1.44f) * conf;
              it[i] = color * (uint8_t)(brightness * 255);
            }
      - addressable_lambda:
          name: "Listening Bright Arc"
          update_interval: 33ms
          lambda: |-
            auto light_color = id(led_ring).current_values;
            Color color(light_color.get_red() * 255, light_color.get_green() * 255,
                  light_color.get_blue() * 255);
            float angle = id(doa_angle);
            float conf = id(doa_confidence);
            int center = ((int)(angle / 15.0f)) % ${led_ring_count};
            for (int i = 0; i < ${led_ring_count}; i++) {
              int diff = i - center;
              if (diff > ${led_ring_count} / 2) diff -= ${led_ring_count};
              if (diff < -${led_ring_count} / 2) diff += ${led_ring_count};
              int abs_diff = diff < 0 ? -diff : diff;
              // Linear taper over 6 LEDs (90 degrees)
              float brightness = (abs_diff <= 6) ? (1.0f - abs_diff / 6.0f) * conf : 0.0f;
              it[i] = color * (uint8_t)(brightness * 255);
            }
      - addressable_lambda:
          name: "Listening Single Point"
          update_interval: 33ms
          lambda: |-
            auto light_color = id(led_ring).current_values;
            Color color(light_color.get_red() * 255, light_color.get_green() * 255,
                  light_color.get_blue() * 255);
            float angle = id(doa_angle);
            float conf = id(doa_confidence);
            int center = ((int)(angle / 15.0f)) % ${led_ring_count};
            for (int i = 0; i < ${led_ring_count}; i++) {
              int diff = i - center;
              if (diff > ${led_ring_count} / 2) diff -= ${led_ring_count};
              if (diff < -${led_ring_count} / 2) diff += ${led_ring_count};
              int abs_diff = diff < 0 ? -diff : diff;
              // Step function: center=100%, +-1=50%, +-2=25%
              float brightness = 0.0f;
              if (abs_diff == 0) brightness = 1.0f;
              else if (abs_diff == 1) brightness = 0.5f;
              else if (abs_diff == 2) brightness = 0.25f;
              it[i] = color * (uint8_t)(brightness * conf * 255);
            }
      - addressable_lambda:
          name: "Listening Gradient Ring"
          update_interval: 33ms
          lambda: |-
            auto light_color = id(led_ring).current_values;
            Color color(light_color.get_red() * 255, light_color.get_green() * 255,
                  light_color.get_blue() * 255);
            float angle = id(doa_angle);
            float conf = id(doa_confidence);
            float center_f = angle / 15.0f;
            float base_brightness = 0.15f;  // minimum so all LEDs stay visible
            for (int i = 0; i < ${led_ring_count}; i++) {
              float diff = i - center_f;
              if (diff > ${led_ring_count} / 2.0f) diff -= ${led_ring_count};
              if (diff < -${led_ring_count} / 2.0f) diff += ${led_ring_count};
              // Cosine curve: peak at source, trough at opposite side
              float cosval = cosf(diff * 3.14159f / (${led_ring_count} / 2.0f));
              float brightness = (base_brightness + (1.0f - base_brightness) * (cosval + 1.0f) / 2.0f) * conf;
              brightness = brightness < base_brightness ? base_brightness * conf : brightness;
              it[i] = color * (uint8_t)(brightness * 255);
            }
```

### Step 2: Modify listening script to select effect based on animation mode

Replace the existing `control_leds_voice_assistant_listening_for_command_phase` script (lines 653-658) with:

```yaml
  - id: control_leds_voice_assistant_listening_for_command_phase
    then:
      - lambda: |-
          std::string mode = id(listening_animation_mode).state;
          std::string effect_name;
          if (mode == "Spotlight Beam") effect_name = "Listening Spotlight Beam";
          else if (mode == "Bright Arc") effect_name = "Listening Bright Arc";
          else if (mode == "Single Point") effect_name = "Listening Single Point";
          else if (mode == "Gradient Ring") effect_name = "Listening Gradient Ring";
          else effect_name = "Listening For Command";
          auto call = id(voice_assistant_leds).turn_on();
          float brightness = id(led_ring).current_values.get_brightness();
          call.set_brightness(brightness > 0.2f ? brightness : 0.2f);
          call.set_effect(effect_name);
          call.perform();
```

### Step 3: Compile to verify

Run: `esphome compile config/satellite1.yaml`
Expected: Successful compilation. Without XMOS mic energy data, `doa_angle` and `doa_confidence` stay at 0, so directional effects will show no LEDs (graceful fallback). "Rotating" mode works exactly as before.

### Step 4: Commit

```bash
git add config/common/led_ring.yaml
git commit -m "feat(leds): add 4 directional listening LED effects with mode selection"
```

---

## Task 3: ESP32 — Extend Satellite1 component to read mic energy and compute DOA

**Files:**
- Modify: `esphome/components/satellite1/satellite1.h:39-48,153-164`
- Modify: `esphome/components/satellite1/satellite1.cpp:31-50`

### Step 1: Extend DC_STATUS_REGISTER enum and add DOA members to satellite1.h

In `satellite1.h`, update the `DC_STATUS_REGISTER` namespace (lines 39-48):

```cpp
namespace DC_STATUS_REGISTER {
enum register_id {
  DEVICE_STATUS = 0,
  GPIO_PORT_IN_A = 1,
  GPIO_PORT_IN_B = 2,
  GPIO_PORT_OUT_A = 3,
  MIC_ENERGY_EAST = 4,
  MIC_ENERGY_WEST = 5,
  MIC_ENERGY_NORTH = 6,
  MIC_ENERGY_SOUTH = 7,

  REGISTER_LEN = 8
};
}
```

Add DOA-related members to the `Satellite1` class. In the `public:` section (after line 74):

```cpp
  float get_doa_angle() const { return this->doa_angle_; }
  float get_doa_confidence() const { return this->doa_confidence_; }
  void set_doa_polling_active(bool active) { this->doa_polling_active_ = active; }
```

In the `protected:` section (after line 161):

```cpp
  float doa_angle_{0.0f};
  float doa_confidence_{0.0f};
  float doa_smooth_x_{0.0f};
  float doa_smooth_y_{0.0f};
  bool doa_polling_active_{false};
  uint32_t last_doa_poll_ms_{0};
```

### Step 2: Add DOA polling and computation to satellite1.cpp

In `satellite1.cpp`, add to the `SAT_XMOS_CONNECTED_STATE` case in `loop()` (currently just a `break` at line 48). Replace lines 46-48:

```cpp
    case SAT_XMOS_CONNECTED_STATE:
      if (this->doa_polling_active_ && (millis() - this->last_doa_poll_ms_ >= 33)) {
        this->last_doa_poll_ms_ = millis();
        if (this->request_status_register_update()) {
          float east = this->dc_status_register_[DC_STATUS_REGISTER::MIC_ENERGY_EAST];
          float west = this->dc_status_register_[DC_STATUS_REGISTER::MIC_ENERGY_WEST];
          float north = this->dc_status_register_[DC_STATUS_REGISTER::MIC_ENERGY_NORTH];
          float south = this->dc_status_register_[DC_STATUS_REGISTER::MIC_ENERGY_SOUTH];

          float x = east - west;
          float y = north - south;

          // Exponential smoothing on (x, y) components to avoid angle wrap discontinuities
          static constexpr float ALPHA = 0.3f;
          this->doa_smooth_x_ = ALPHA * x + (1.0f - ALPHA) * this->doa_smooth_x_;
          this->doa_smooth_y_ = ALPHA * y + (1.0f - ALPHA) * this->doa_smooth_y_;

          float magnitude = sqrtf(this->doa_smooth_x_ * this->doa_smooth_x_ +
                                  this->doa_smooth_y_ * this->doa_smooth_y_);

          // Only update angle if above noise floor
          if (magnitude > 10.0f) {
            float angle_rad = atan2f(this->doa_smooth_x_, this->doa_smooth_y_);
            float angle_deg = angle_rad * 180.0f / 3.14159265f;
            if (angle_deg < 0) angle_deg += 360.0f;
            this->doa_angle_ = angle_deg;
          }

          // Normalize confidence: 0-255 range per mic, max magnitude ~360
          this->doa_confidence_ = std::min(1.0f, magnitude / 180.0f);
        }
      }
      break;
```

Add `#include <cmath>` at the top of `satellite1.cpp` (after line 2).

### Step 3: Wire DOA polling activation and global updates from YAML

The DOA globals and polling need to be wired from the voice assistant phase scripts. In `config/common/led_ring.yaml`, update the `control_leds_voice_assistant_listening_for_command_phase` script (from Task 2) to activate DOA polling:

At the start of the listening script lambda, add:
```cpp
id(satellite1_id).set_doa_polling_active(true);
```

We also need a way to stop polling when not listening. Add to the start of the waiting, thinking, replying, idle, and error scripts:
```cpp
id(satellite1_id).set_doa_polling_active(false);
```

For the DOA globals to update from the component, add a `PollingComponent` interval or use the existing loop. Since `loop()` already runs every cycle, add global updates at the end of the DOA computation block (inside the `doa_polling_active_` check):

Actually, the simplest approach is to update the globals directly from the LED effect lambdas, not from the component. The effects already run at 33ms intervals. Instead, have each directional effect lambda call the component to get the latest values:

In each directional effect lambda, the first lines should be:
```cpp
// Trigger a status register poll for fresh mic energy data
id(satellite1_id).set_doa_polling_active(true);
```

And in the `control_leds` script for non-listening phases, set it to false:
```cpp
id(satellite1_id).set_doa_polling_active(false);
```

The DOA data flows: XMOS → SPI status register → `satellite1.loop()` → `doa_angle_`/`doa_confidence_` members → lambdas access via `id(satellite1_id).get_doa_angle()`.

**Update the effect lambdas** from Task 2 to use the component getters instead of globals. Replace `id(doa_angle)` with `id(satellite1_id).get_doa_angle()` and `id(doa_confidence)` with `id(satellite1_id).get_doa_confidence()` in all 4 directional effect lambdas.

Alternatively, keep the globals and have the component update them in loop(). Add to the DOA computation block:

```cpp
// Publish to globals for YAML lambda access
// (requires globals to be declared - see Task 1)
```

**Decision:** Use the component's public getters directly in the lambdas. Remove the `doa_angle` and `doa_confidence` globals from Task 1 since they're redundant — the component holds the data.

### Step 4: Compile to verify

Run: `esphome compile config/satellite1.yaml`
Expected: Successful compilation. DOA polling is inactive by default, so no SPI overhead when not listening.

### Step 5: Commit

```bash
git add esphome/components/satellite1/satellite1.h esphome/components/satellite1/satellite1.cpp config/common/led_ring.yaml
git commit -m "feat(satellite1): add mic energy SPI polling and DOA computation"
```

---

## Task 4: XMOS — Fork repo, enable 4 mics, decouple pipeline channels

**Repo:** `/home/jeremy/GitHub/FutureProofHomes/Satellite1-XMOS` (create fork first)

**Files:**
- Modify: `satellite-xmos-firmware/bsp_config/SATELLITE1/SATELLITE1.cmake:42-43,60-61`
- Modify: `satellite-xmos-firmware/src/app_conf.h:26`

### Step 1: Fork the Satellite1-XMOS repository

```bash
cd /home/jeremy/GitHub/FutureProofHomes/Satellite1-XMOS
# Create a feature branch (or fork via GitHub)
git checkout -b feature/4-mic-doa
```

### Step 2: Change mic mapping to include all 4 mics

In `satellite-xmos-firmware/bsp_config/SATELLITE1/SATELLITE1.cmake`, update lines 42-43:

```cmake
# use East as first, West as second, North as third, South as fourth mic
set(MIC_MAPPING "4, 5, 0, 1")
```

Update line 60:

```cmake
        MIC_ARRAY_CONFIG_MIC_COUNT=4
```

### Step 3: Decouple audio pipeline channels from mic count

In `satellite-xmos-firmware/src/app_conf.h`, change line 26 from:

```c
#define appconfAUDIO_PIPELINE_CHANNELS          MIC_ARRAY_CONFIG_MIC_COUNT
```

to:

```c
#define appconfAUDIO_PIPELINE_CHANNELS          2
```

This keeps the entire AEC/IC/NS/AGC pipeline at 2 channels while the mic array now outputs 4.

### Step 4: Compile to verify

```bash
# Build using the project's build system
cd satellite-xmos-firmware
cmake -B build -DBOARD=SATELLITE1
make -C build -j$(nproc)
```

Expected: May fail because `audio_pipeline_input()` in `main.c` now receives 4 channels from `rtos_mic_array_rx` but writes into a 2-channel buffer. This is addressed in Task 5.

### Step 5: Commit

```bash
git add bsp_config/SATELLITE1/SATELLITE1.cmake src/app_conf.h
git commit -m "feat: enable all 4 mics, decouple pipeline channels from mic count"
```

---

## Task 5: XMOS — Add per-mic RMS computation and SPI status register output

**Files:**
- Modify: `satellite-xmos-firmware/audio_pipelines/reference/adec/audio_pipeline_dsp.h:38-48`
- Modify: `satellite-xmos-firmware/src/main.c:147-199`
- Modify: `satellite-xmos-firmware/audio_pipelines/reference/adec/audio_pipeline_t1.c:38-55`
- Modify: `satellite-xmos-firmware/audio_pipelines/reference/adec/audio_pipeline_t0.c:67-74`

### Step 1: Add mic_rms field to frame_data_t

In `audio_pipeline_dsp.h`, add a field to `frame_data_t` (after line 41, before `vnr_pred_flag`):

```c
    int32_t mic_samples_passthrough[appconfAUDIO_PIPELINE_CHANNELS][appconfAUDIO_PIPELINE_FRAME_ADVANCE];

    /* Per-mic RMS energy (0-255), computed from raw 4-mic data before AEC */
    uint8_t mic_rms[4]; /* East, West, North, South */

    /* Below is additional context needed by other stages on a per frame basis */
```

### Step 2: Add global for RMS and modify audio_pipeline_input in main.c

In `main.c`, add a static buffer and RMS globals after line 38 (`volatile int aec_ref_source`):

```c
/* 4-channel mic buffer: receives all mics from mic array, only East/West fed to pipeline */
static int32_t mic_buf_4ch[4 * appconfAUDIO_PIPELINE_FRAME_ADVANCE];

/* Per-mic RMS energy (0-255), set in audio_pipeline_input, read by audio_pipeline_input_i on Tile 1 */
volatile uint8_t g_mic_rms[4] = {0, 0, 0, 0};
```

Replace the `audio_pipeline_input` function (lines 147-199) with:

```c
void audio_pipeline_input(void *input_app_data,
                        int32_t **input_audio_frames,
                        size_t ch_count,
                        size_t frame_count)
{
    (void) input_app_data;
    int32_t **mic_ptr = (int32_t **)(input_audio_frames + (2 * frame_count));

    static int flushed;
    while (!flushed) {
        size_t received;
        received = rtos_mic_array_rx(mic_array_ctx,
                                     (int32_t **)mic_buf_4ch,
                                     frame_count,
                                     0);
        if (received == 0) {
            rtos_mic_array_rx(mic_array_ctx,
                              (int32_t **)mic_buf_4ch,
                              frame_count,
                              portMAX_DELAY);
            flushed = 1;
        }
    }

#if ON_TILE(SPEAKER_PIPELINE_TILE_NO)
    //read the speaker pipeline output as reference
    void *frame_data;
    (void) rtos_osal_queue_receive(ref_input_queue, &frame_data, RTOS_OSAL_WAIT_FOREVER);
    int32_t *tmpptr = (int32_t *)input_audio_frames;
    int32_t *refptr = (int32_t *)frame_data;

    for (int i=0; i<frame_count; i++) {
        /* ref is first */
        *(tmpptr + i) = *(refptr++);
        *(tmpptr + i + frame_count) = *(refptr++);
    }

    rtos_osal_free(frame_data);
#endif

    /* Receive all 4 mic channels into local buffer */
    rtos_mic_array_rx(mic_array_ctx,
                      (int32_t **)mic_buf_4ch,
                      frame_count,
                      portMAX_DELAY);

    /* Copy only East (ch 0) and West (ch 1) into the 2-channel pipeline buffer */
    memcpy(mic_ptr, &mic_buf_4ch[0], 2 * frame_count * sizeof(int32_t));

    /* Compute per-mic RMS energy and scale to 8-bit */
    for (int ch = 0; ch < 4; ch++) {
        int64_t sum_sq = 0;
        int32_t *ch_samples = &mic_buf_4ch[ch * frame_count];
        for (int s = 0; s < frame_count; s++) {
            int64_t sample = (int64_t)ch_samples[s] >> 16; /* scale down to avoid overflow */
            sum_sq += sample * sample;
        }
        uint32_t rms = (uint32_t)(sum_sq / frame_count);
        /* Approximate sqrt via iterative method, then scale to 0-255 */
        uint32_t root = 0;
        uint32_t bit = 1UL << 30;
        while (bit > rms) bit >>= 2;
        while (bit != 0) {
            if (rms >= root + bit) {
                rms -= root + bit;
                root = (root >> 1) + bit;
            } else {
                root >>= 1;
            }
            bit >>= 2;
        }
        /* Scale: root is now ~RMS. Clamp to 0-255 with headroom tuning */
        uint8_t energy = (root > 255) ? 255 : (uint8_t)root;
        g_mic_rms[ch] = energy;
    }
}
```

### Step 3: Copy RMS into frame_data on Tile 1

In `audio_pipeline_t1.c`, in `audio_pipeline_input_i()` (line 38-55), add RMS copy after line 52:

Add at the top of the file (after the includes):
```c
extern volatile uint8_t g_mic_rms[4];
```

After the `memcpy(frame_data->samples, ...)` line (line 52), add:

```c
    memcpy(frame_data->mic_rms, (const uint8_t *)g_mic_rms, 4);
```

### Step 4: Write RMS to status registers on Tile 0

In `audio_pipeline_t0.c`, modify `audio_pipeline_output_i()` (lines 67-74) to write mic energy to status registers before calling audio_pipeline_output:

```c
static int audio_pipeline_output_i(frame_data_t *frame_data,
                                   void *output_app_data)
{
    /* Write per-mic RMS energy to SPI status registers 4-7 */
    for (int i = 0; i < 4; i++) {
        device_control_set_resource_status(device_control_spi_ctx, 4 + i, frame_data->mic_rms[i]);
    }

    return audio_pipeline_output(output_app_data,
                               (int32_t **)frame_data->samples,
                               6,
                               appconfAUDIO_PIPELINE_FRAME_ADVANCE);
}
```

Add the required include at the top of the file (after existing includes):
```c
#include "device_control.h"
```

Note: `device_control_spi_ctx` is declared as `extern` in `platform/driver_instances.h`. Verify it's accessible on Tile 0 (it should be — the SPI device control runs on Tile 0).

### Step 5: Compile to verify

```bash
cmake -B build -DBOARD=SATELLITE1
make -C build -j$(nproc)
```

Expected: Successful compilation. The 4-mic RMS data now flows: PDM → mic_array → RMS computation → frame_data → intertile → status register → SPI.

### Step 6: Commit

```bash
git add audio_pipelines/reference/adec/audio_pipeline_dsp.h \
        audio_pipelines/reference/adec/audio_pipeline_t0.c \
        audio_pipelines/reference/adec/audio_pipeline_t1.c \
        src/main.c
git commit -m "feat: compute per-mic RMS energy and expose via SPI status registers"
```

---

## Task 6: Integration testing and tuning

### Step 1: Flash XMOS firmware

Flash the updated XMOS firmware to the Satellite1 device using the project's flashing procedure.

### Step 2: Update ESP32 external components reference

If the XMOS firmware version changed, update the version reference in `config/common/external_components.yaml` or wherever the XMOS firmware version is configured (currently `xmos_firmware_version: v1.0.3`).

### Step 3: Flash ESP32 firmware

```bash
esphome upload config/satellite1.yaml
```

### Step 4: Verify SPI communication

Stream device logs:
```bash
esphome logs config/satellite1.yaml
```

Speak toward the device from different directions and verify:
- XMOS connects successfully (existing behavior preserved)
- No SPI errors when DOA polling activates during listening
- DOA angle changes when speaking from different directions

### Step 5: Tune parameters

Parameters that may need tuning after initial testing:

1. **RMS scaling** (XMOS `main.c`): The `>> 16` shift and 0-255 clamping may need adjustment based on actual mic levels. If values are always 0 or always 255, adjust the shift amount.

2. **Smoothing alpha** (ESP32 `satellite1.cpp`): `ALPHA = 0.3f` — increase for faster tracking (more jitter), decrease for smoother (more lag). Range: 0.1 (very smooth) to 0.8 (very responsive).

3. **Noise floor threshold** (ESP32 `satellite1.cpp`): `magnitude > 10.0f` — adjust based on ambient noise levels. Too low = jitter in silence. Too high = unresponsive.

4. **Confidence normalization** (ESP32 `satellite1.cpp`): `magnitude / 180.0f` — adjust so normal speech gives confidence 0.5-0.9 range.

5. **LED effect parameters**: Gaussian sigma, arc width, gradient base brightness — adjust per visual preference.

### Step 6: Commit any tuning changes

```bash
git add -A
git commit -m "tune: adjust DOA parameters after hardware testing"
```

---

## Architecture Reference

### Data Flow

```
XMOS Tile 1 (PDM → 4-mic decimation → RMS → frame_data)
    ↓ intertile_tx (frame_data_t with mic_rms[4])
XMOS Tile 0 (receive frame_data → write mic_rms to status registers 4-7)
    ↓ SPI (status register piggybacked on response)
ESP32 Satellite1::loop() (poll @ 30Hz → read registers → vector sum → smooth)
    ↓ doa_angle_, doa_confidence_
LED effect lambdas (render selected animation mode)
    ↓
24 WS2812 LEDs
```

### Mic-to-LED Mapping

```
        North (mic ch 2, LED 0, 0°)
           |
West ------+------ East (mic ch 0, LED 6, 90°)
(ch 1,     |
LED 18,    |
270°)   South (mic ch 3, LED 12, 180°)
```

DOA angle: `atan2(east - west, north - south)` → 0° = North (LED 0), 90° = East (LED 6).
LED index: `round(angle / 15) % 24`.

### Key Constants

| Constant | Value | Location |
|----------|-------|----------|
| LED count | 24 | `${led_ring_count}` substitution |
| Degrees per LED | 15° | 360 / 24 |
| DOA poll interval | 33ms (~30Hz) | satellite1.cpp |
| LED update interval | 33ms (~30fps) | effect lambdas |
| Smoothing alpha | 0.3 | satellite1.cpp |
| Noise floor threshold | 10.0 | satellite1.cpp |
| RMS window | 240 samples (15ms @ 16kHz) | One audio frame |
| Status register indices | 4-7 | East, West, North, South |
