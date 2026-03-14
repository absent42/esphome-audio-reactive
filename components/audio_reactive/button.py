"""Button platform for audio_reactive component."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import AudioReactiveComponent, audio_reactive_ns

CONF_AUDIO_REACTIVE_ID = "audio_reactive_id"
CONF_RESET_AGC = "reset_agc"

AudioReactiveResetAGCButton = audio_reactive_ns.class_(
    "AudioReactiveResetAGCButton", button.Button, cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_AUDIO_REACTIVE_ID): cv.use_id(AudioReactiveComponent),
        cv.Optional(CONF_RESET_AGC): button.button_schema(
            AudioReactiveResetAGCButton,
            icon="mdi:refresh",
            entity_category=ENTITY_CATEGORY_CONFIG,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_AUDIO_REACTIVE_ID])

    if CONF_RESET_AGC in config:
        btn = await button.new_button(config[CONF_RESET_AGC])
        await cg.register_component(btn, config[CONF_RESET_AGC])
        cg.add(btn.set_parent(parent))
        cg.add(parent.set_reset_agc_button(btn))
