import esphome.codegen as cg
from esphome.components import esp32, light, speaker
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

# Shared schema: each effect auto-discovers the single AudioVisualizerSpeaker.
VISUALIZER_EFFECT_SCHEMA = {
    cv.GenerateID(CONF_VISUALIZER): cv.use_id(AudioVisualizerSpeaker),
}


async def _visualizer_effect_to_code(config, effect_id):
    """Shared codegen for all visualizer effects."""
    var = cg.new_Pvariable(effect_id, config[CONF_NAME])
    viz = await cg.get_variable(config[CONF_VISUALIZER])
    cg.add(var.set_visualizer(viz))
    cg.add(var.set_update_interval(33))
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
