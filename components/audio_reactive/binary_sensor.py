"""Binary sensor platform for audio_reactive component."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import CONF_ID

from . import AudioReactiveComponent, audio_reactive_ns

CONF_AUDIO_REACTIVE_ID = "audio_reactive_id"
CONF_ONSET_DETECTED = "onset_detected"
CONF_SILENCE = "silence"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_AUDIO_REACTIVE_ID): cv.use_id(AudioReactiveComponent),
        cv.Optional(CONF_ONSET_DETECTED): binary_sensor.binary_sensor_schema(
            icon="mdi:music-note",
        ),
        cv.Optional(CONF_SILENCE): binary_sensor.binary_sensor_schema(
            icon="mdi:volume-off",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_AUDIO_REACTIVE_ID])

    if CONF_ONSET_DETECTED in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_ONSET_DETECTED])
        cg.add(parent.set_onset_binary_sensor(sens))

    if CONF_SILENCE in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_SILENCE])
        cg.add(parent.set_silence_binary_sensor(sens))
