"""ESPHome audio_reactive component."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@absent42"]
DEPENDENCIES = ["microphone"]
AUTO_LOAD = ["number"]

audio_reactive_ns = cg.esphome_ns.namespace("audio_reactive")
AudioReactiveComponent = audio_reactive_ns.class_(
    "AudioReactiveComponent", cg.Component
)

CONF_MICROPHONE = "microphone"
CONF_UPDATE_INTERVAL = "update_interval"
CONF_BEAT_SENSITIVITY = "beat_sensitivity"

from esphome.components import microphone

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(AudioReactiveComponent),
        cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
        cv.Optional(CONF_UPDATE_INTERVAL, default="50ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_BEAT_SENSITIVITY, default=50): cv.int_range(min=1, max=100),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    cg.add_library("arduinoFFT", "2.0.4")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    mic = await cg.get_variable(config[CONF_MICROPHONE])
    cg.add(var.set_microphone(mic))
    cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL]))
    cg.add(var.set_beat_sensitivity(config[CONF_BEAT_SENSITIVITY]))
