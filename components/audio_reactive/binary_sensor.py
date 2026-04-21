"""Binary sensor platform for audio_reactive component."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import CONF_ID

from . import AudioReactiveComponent, CONF_AUDIO_REACTIVE_ID, resolve_tier_from_core, DSP_TIER_PRO

CONF_ONSET_DETECTED = "onset_detected"
CONF_SILENCE = "silence"
CONF_CALIBRATION_STALE = "calibration_stale"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_AUDIO_REACTIVE_ID): cv.use_id(AudioReactiveComponent),
        cv.Optional(CONF_ONSET_DETECTED): binary_sensor.binary_sensor_schema(
            icon="mdi:music-note",
        ),
        cv.Optional(CONF_SILENCE): binary_sensor.binary_sensor_schema(
            icon="mdi:volume-off",
        ),
        cv.Optional(CONF_CALIBRATION_STALE): binary_sensor.binary_sensor_schema(),
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

    if CONF_CALIBRATION_STALE in config:
        tier = resolve_tier_from_core()
        if tier != DSP_TIER_PRO:
            raise cv.Invalid(
                "binary_sensor.calibration_stale requires dsp_tier: pro. "
                "This diagnostic sensor is only available on ESP32-S3 + PSRAM boards."
            )
        sens = await binary_sensor.new_binary_sensor(config[CONF_CALIBRATION_STALE])
        cg.add(parent.set_calibration_stale_binary_sensor(sens))
