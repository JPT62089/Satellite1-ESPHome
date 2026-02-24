import esphome.codegen as cg
from esphome.components import esp32, light, number, speaker, switch
from esphome.components.light.effects import register_addressable_effect
from esphome.components.light.types import AddressableLightEffect
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_NAME,
    CONF_OUTPUT_SPEAKER,
    PLATFORM_ESP32,
)

AUTO_LOAD = ["audio"]
DEPENDENCIES = ["speaker", "light"]

audio_visualizer_ns = cg.esphome_ns.namespace("audio_visualizer")
AudioVisualizerSpeaker = audio_visualizer_ns.class_(
    "AudioVisualizerSpeaker", cg.Component, speaker.Speaker
)

# Effect classes declared in visualizer_effect.h
SpectrumRingEffect = audio_visualizer_ns.class_(
    "SpectrumRingEffect", AddressableLightEffect
)
PulseBeatEffect = audio_visualizer_ns.class_(
    "PulseBeatEffect", AddressableLightEffect
)
VUSweepEffect = audio_visualizer_ns.class_(
    "VUSweepEffect", AddressableLightEffect
)
WaveformOrbitEffect = audio_visualizer_ns.class_(
    "WaveformOrbitEffect", AddressableLightEffect
)

CONF_VISUALIZER = "visualizer"
CONF_VIZ_SPEED = "speed"
CONF_VIZ_INTENSITY = "intensity"
CONF_VIZ_REVERSE = "reverse"
CONF_VIZ_MIRROR = "mirror"
CONF_VIZ_START = "start"

# Shared schema: each effect auto-discovers the single AudioVisualizerSpeaker.
VISUALIZER_EFFECT_SCHEMA = {
    cv.GenerateID(CONF_VISUALIZER): cv.use_id(AudioVisualizerSpeaker),
    cv.Optional(CONF_VIZ_SPEED): cv.use_id(number.Number),
    cv.Optional(CONF_VIZ_INTENSITY): cv.use_id(number.Number),
    cv.Optional(CONF_VIZ_REVERSE): cv.use_id(switch.Switch),
    cv.Optional(CONF_VIZ_MIRROR): cv.use_id(switch.Switch),
    cv.Optional(CONF_VIZ_START): cv.use_id(number.Number),
}


async def _visualizer_effect_to_code(config, effect_id):
    """Shared codegen for all visualizer effects."""
    var = cg.new_Pvariable(effect_id, config[CONF_NAME])
    viz = await cg.get_variable(config[CONF_VISUALIZER])
    cg.add(var.set_visualizer(viz))
    # Note: set_update_interval() intentionally omitted — speed entity drives
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


@register_addressable_effect(
    "visualizer_spectrum",
    SpectrumRingEffect,
    "Visualizer Spectrum",
    VISUALIZER_EFFECT_SCHEMA,
)
async def visualizer_spectrum_to_code(config, effect_id):
    return await _visualizer_effect_to_code(config, effect_id)


@register_addressable_effect(
    "visualizer_pulse",
    PulseBeatEffect,
    "Visualizer Pulse",
    VISUALIZER_EFFECT_SCHEMA,
)
async def visualizer_pulse_to_code(config, effect_id):
    return await _visualizer_effect_to_code(config, effect_id)


@register_addressable_effect(
    "visualizer_vu_sweep",
    VUSweepEffect,
    "Visualizer VU Sweep",
    VISUALIZER_EFFECT_SCHEMA,
)
async def visualizer_vu_sweep_to_code(config, effect_id):
    return await _visualizer_effect_to_code(config, effect_id)


@register_addressable_effect(
    "visualizer_waveform",
    WaveformOrbitEffect,
    "Visualizer Waveform",
    VISUALIZER_EFFECT_SCHEMA,
)
async def visualizer_waveform_to_code(config, effect_id):
    return await _visualizer_effect_to_code(config, effect_id)


# --- Speaker platform config ------------------------------------------------

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
