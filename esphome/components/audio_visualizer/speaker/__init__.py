import esphome.codegen as cg
from esphome.components import esp32, speaker
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_OUTPUT_SPEAKER,
    PLATFORM_ESP32,
)

AUTO_LOAD = ["audio"]
DEPENDENCIES = ["speaker"]

audio_visualizer_ns = cg.esphome_ns.namespace("audio_visualizer")
AudioVisualizerSpeaker = audio_visualizer_ns.class_(
    "AudioVisualizerSpeaker", cg.Component, speaker.Speaker
)

CONF_LIGHT = "light"

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
