"""Switch platform for audio_reactive component."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import AudioReactiveComponent, audio_reactive_ns

CONF_AUDIO_REACTIVE_ID = "audio_reactive_id"
CONF_MICROPHONE_MUTE = "microphone_mute"

AudioReactiveMicrophoneMuteSwitch = audio_reactive_ns.class_(
    "AudioReactiveMicrophoneMuteSwitch", switch.Switch, cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_AUDIO_REACTIVE_ID): cv.use_id(AudioReactiveComponent),
        cv.Optional(CONF_MICROPHONE_MUTE): switch.switch_schema(
            AudioReactiveMicrophoneMuteSwitch,
            icon="mdi:microphone-off",
            entity_category=ENTITY_CATEGORY_CONFIG,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_AUDIO_REACTIVE_ID])

    if CONF_MICROPHONE_MUTE in config:
        sw = await switch.new_switch(config[CONF_MICROPHONE_MUTE])
        await cg.register_component(sw, config[CONF_MICROPHONE_MUTE])
        cg.add(sw.set_parent(parent))
        cg.add(parent.set_mute_switch(sw))
