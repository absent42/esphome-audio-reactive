"""Binary sensor platform for audio_reactive component."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import CONF_ID

from . import AudioReactiveComponent, audio_reactive_ns

CONF_AUDIO_REACTIVE_ID = "audio_reactive_id"
CONF_BEAT_DETECTED = "beat_detected"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_AUDIO_REACTIVE_ID): cv.use_id(AudioReactiveComponent),
        cv.Optional(CONF_BEAT_DETECTED): binary_sensor.binary_sensor_schema(),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_AUDIO_REACTIVE_ID])

    if CONF_BEAT_DETECTED in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_BEAT_DETECTED])
        cg.add(parent.set_beat_binary_sensor(sens))
