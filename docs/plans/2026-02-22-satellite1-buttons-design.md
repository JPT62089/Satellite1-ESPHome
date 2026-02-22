# Satellite1 Buttons Component Design

## Problem

The current button implementation in `config/common/buttons.yaml` uses ESPHome's generic `binary_sensor` platform with `on_multi_click` for gesture detection. This has several issues:

- **Readability**: Deep nesting of timing blocks, lambda guards (`init_in_progress`), and inline logic make the YAML hard to follow.
- **Maintainability**: Button state tracking uses scattered globals (`volume_buttons_touched`, `action_button_touched`, `factory_reset_requested`).
- **Missing features**: No hold-to-repeat for volume, no button combo support, no efficient batched SPI polling.

## Approach

Create a custom `satellite1_buttons` C++ component that handles all button input processing, exposing clean ESPHome triggers to YAML.

### Alternatives Considered

- **Better-structured YAML only**: Doesn't solve the complexity of multi-click detection or enable hold-to-repeat/combos.
- **Full component + HA-configurable actions**: Runtime button remapping from HA UI adds significant complexity for uncertain value. Deferred to future iteration.

## Architecture

### File Structure

```
esphome/components/satellite1/buttons/
  __init__.py               # YAML schema, code generation
  satellite1_buttons.h      # ButtonManager + Button classes
  satellite1_buttons.cpp    # Implementation
```

### Key Classes

- **`Satellite1ButtonManager`** — inherits `Component` + `Satellite1SPIService`. One instance manages all buttons. Polls all XMOS button pins in a single SPI read per loop cycle. Owns gesture detection state machines.
- **`Satellite1Button`** — per-button config and state. Not a standalone ESPHome component; owned by the manager.

### Data Flow

```
XMOS (SPI) → ButtonManager::loop() polls INPUT_A register once
  → per-button debounce filter
  → gesture state machine (click/multi-click/hold/combo)
  → fire ESPHome Trigger callbacks
  → YAML automation actions execute
```

## Gesture Detection

### Debounce
Each button has a configurable debounce window (default 20ms). Raw pin state changes are ignored until stable for the debounce duration.

### Click Detection (single/double/triple)
- After a press-release, start a multi-click window timer (default 250ms).
- Each additional press-release within the window increments the click count.
- When the window expires, fire the corresponding trigger.
- Max click count is configurable (default 3).

### Hold / Hold-to-Repeat
- If a button stays pressed past the hold threshold (default 1s), fire `on_hold`.
- For volume buttons, `repeat_interval` (default 150ms) fires `on_hold_repeat` continuously while held.
- Hold and multi-click are mutually exclusive — once hold fires, no click event on release.

### Long Press Thresholds
- Multiple hold thresholds per button (e.g., 1s, 10s, 22s for the factory reset flow).
- Each threshold fires its trigger progressively as the hold duration increases.

### Button Combos
- The manager tracks simultaneously pressed buttons.
- A combo is recognized when a specific set of buttons are all held for a configurable duration (default 500ms).
- Combos are defined in YAML as lists of button IDs.

### Timing Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `debounce` | 20ms | Debounce filter |
| `multi_click_window` | 250ms | Time to wait for additional clicks |
| `hold_threshold` | 1000ms | Duration before `on_hold` fires |
| `repeat_interval` | 150ms | Interval for hold-to-repeat events |

## YAML Schema

```yaml
satellite1_buttons:
  id: button_manager
  debounce: 20ms
  multi_click_window: 250ms

  buttons:
    - id: btn_volume_up
      port: INPUT_A
      pin: 0
      name: "Volume Up"
      icon: "mdi:volume-plus"
      on_single_click:
        - script.execute:
            id: control_volume
            increase_volume: true
      on_hold_repeat:
        repeat_interval: 150ms
        then:
          - script.execute:
              id: control_volume
              increase_volume: true

    - id: btn_volume_down
      port: INPUT_A
      pin: 2
      name: "Volume Down"
      icon: "mdi:volume-minus"
      on_single_click:
        - script.execute:
            id: control_volume
            increase_volume: false
      on_hold_repeat:
        repeat_interval: 150ms
        then:
          - script.execute:
              id: control_volume
              increase_volume: false

    - id: btn_mute
      port: INPUT_A
      pin: 3
      name: "Hardware Mute"
      icon: "mdi:microphone-off"
      on_single_click:
        - switch.toggle: master_mute_switch

    - id: btn_action
      gpio: 0  # ESP32 native GPIO, not XMOS SPI
      name: "Action"
      icon: "mdi:gesture-tap"
      on_press:
        - script.execute: control_leds
      on_release:
        - script.execute: control_leds
      on_single_click:
        - script.execute: handle_single_press
        - event.trigger:
            id: action_button_press_event
            event_type: "single_press"
      on_double_click:
        - script.execute:
            id: play_sound
            priority: false
            sound_file: !lambda return id(center_button_double_press_sound);
        - event.trigger:
            id: action_button_press_event
            event_type: "double_press"
      on_triple_click:
        - script.execute:
            id: play_sound
            priority: false
            sound_file: !lambda return id(center_button_triple_press_sound);
        - event.trigger:
            id: action_button_press_event
            event_type: "triple_press"
      on_hold:
        - threshold: 1s
          then:
            - script.execute:
                id: play_sound
                priority: false
                sound_file: !lambda return id(center_button_long_press_sound);
            - light.turn_off: voice_assistant_leds
            - event.trigger:
                id: action_button_press_event
                event_type: "long_press"
        - threshold: 10s
          then:
            - light.turn_on:
                brightness: 100%
                id: voice_assistant_leds
                effect: "Factory Reset Coming Up"
            - script.execute:
                id: play_sound
                priority: true
                sound_file: !lambda return id(factory_reset_initiated_sound);
        - threshold: 22s
          then:
            - if:
                condition:
                  lambda: return id(xflash).flash_accessible();
                then:
                  - lambda: id(factory_reset_requested) = true;
                  - memory_flasher.erase:
                else:
                  - button.press: factory_reset_button

  combos:
    - buttons: [btn_volume_up, btn_volume_down]
      hold_duration: 500ms
      on_combo:
        - logger.log: "Volume combo pressed"
```

## C++ Design

### ButtonConfig

```cpp
struct HoldThreshold {
  uint32_t duration_ms;
  Trigger<> *trigger;
};

struct ButtonConfig {
  uint8_t pin;
  XMOSPort port;
  bool use_native_gpio;     // true for ESP32 GPIO (btn_action)
  uint32_t debounce_ms;
  uint32_t multi_click_window_ms;
  std::vector<HoldThreshold> hold_thresholds;  // sorted by duration
  uint32_t repeat_interval_ms;  // 0 = no repeat
};
```

### Satellite1Button

Holds per-button config and state machine:
- Debounced state tracking
- Click counter with multi-click window timer
- Hold duration tracking with progressive threshold firing
- Hold-to-repeat timer
- Triggers: `on_press`, `on_release`, `on_single_click`, `on_double_click`, `on_triple_click`, `on_hold_repeat`

### Satellite1ButtonManager

```cpp
class Satellite1ButtonManager : public Component, public Satellite1SPIService {
  std::vector<Satellite1Button *> buttons_;
  std::vector<ComboConfig> combos_;
  bool enabled_ = false;  // enabled on XMOS connected

  void setup() override;
  void loop() override;
  float get_setup_priority() const override;

  void poll_buttons_();
  void process_button_(Satellite1Button *btn, bool raw_state);
  void check_combos_();
};
```

### Loop Logic

1. If not enabled, return.
2. Single SPI read: `request_status_register_update()`, read `GPIO_PORT_IN_A`.
3. For each button: read raw state (SPI register bit or native GPIO), call `process_button_()`.
4. Call `check_combos_()`.

### State Machine (per button)

1. Apply debounce: ignore state changes shorter than `debounce_ms`.
2. Detect edges: fire `on_press` / `on_release` triggers.
3. On press: record start time.
4. While held: check hold thresholds progressively, fire `on_hold_repeat` at interval.
5. On release: if no hold fired, increment click count, start multi-click window.
6. On window expiry: fire click trigger based on count, reset.

## Error Handling

- **SPI failures**: Hold last known button states rather than treating all as released. Log warning at most once per second.
- **Init guard**: Manager starts disabled, enables on `on_xmos_connected` callback. Replaces scattered `init_in_progress` lambda checks.
- **Factory reset safety**: The `xflash.flash_accessible()` check remains in the YAML action, not the C++ component.

## Testing

- **Compile test**: `tests/components/satellite1_buttons/test_buttons.yaml` validates the component compiles with all gesture types.
- **Debug logging**: All gesture detections logged at DEBUG level for manual verification via `esphome logs`.
- **Regression**: Existing button behaviors (action button single/double/triple/long, volume step, mute toggle) must work identically after refactor.

## Migration

Clean replacement: the old `buttons.yaml` with `binary_sensor` + `on_multi_click` is replaced by the new `satellite1_buttons` config block. The globals `volume_buttons_touched` and `action_button_touched` move into the manager as internal state, exposed as read-only properties for LED control.
