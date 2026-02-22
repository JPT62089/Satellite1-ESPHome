import esphome.codegen as cg
from esphome.components import esp32, light, speaker
from esphome.components.light.types import AddressableLightEffect
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
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

CONF_LIGHT = "light"
CONF_SPECTRUM_ID = "spectrum_effect_id"
CONF_PULSE_ID = "pulse_effect_id"
CONF_VU_ID = "vu_effect_id"
CONF_WAVEFORM_ID = "waveform_effect_id"

CONFIG_SCHEMA = cv.All(
    speaker.SPEAKER_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(AudioVisualizerSpeaker),
            cv.Required(CONF_OUTPUT_SPEAKER): cv.use_id(speaker.Speaker),
            cv.Required(CONF_LIGHT): cv.use_id(light.LightState),
            cv.GenerateID(CONF_SPECTRUM_ID): cv.declare_id(SpectrumRingEffect),
            cv.GenerateID(CONF_PULSE_ID): cv.declare_id(PulseBeatEffect),
            cv.GenerateID(CONF_VU_ID): cv.declare_id(VUSweepEffect),
            cv.GenerateID(CONF_WAVEFORM_ID): cv.declare_id(WaveformOrbitEffect),
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

    light_var = await cg.get_variable(config[CONF_LIGHT])

    # Create the 4 visualizer effects
    spectrum = cg.new_Pvariable(config[CONF_SPECTRUM_ID], "Visualizer Spectrum")
    cg.add(spectrum.set_visualizer(var))
    cg.add(spectrum.set_update_interval(33))

    pulse = cg.new_Pvariable(config[CONF_PULSE_ID], "Visualizer Pulse")
    cg.add(pulse.set_visualizer(var))
    cg.add(pulse.set_update_interval(33))

    vu = cg.new_Pvariable(config[CONF_VU_ID], "Visualizer VU Sweep")
    cg.add(vu.set_visualizer(var))
    cg.add(vu.set_update_interval(33))

    waveform = cg.new_Pvariable(config[CONF_WAVEFORM_ID], "Visualizer Waveform")
    cg.add(waveform.set_visualizer(var))
    cg.add(waveform.set_update_interval(33))

    # Add all effects to the light
    cg.add(light_var.add_effects([spectrum, pulse, vu, waveform]))
