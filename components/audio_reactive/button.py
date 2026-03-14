"""Button platform for audio_reactive component."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import AudioReactiveComponent, audio_reactive_ns

CONF_AUDIO_REACTIVE_ID = "audio_reactive_id"
CONF_RESET_AGC = "reset_agc"
CONF_CALIBRATE_QUIET = "calibrate_quiet"
CONF_CALIBRATE_MUSIC = "calibrate_music"

AudioReactiveResetAGCButton = audio_reactive_ns.class_(
    "AudioReactiveResetAGCButton", button.Button, cg.Component
)
AudioReactiveCalibrateQuietButton = audio_reactive_ns.class_(
    "AudioReactiveCalibrateQuietButton", button.Button, cg.Component
)
AudioReactiveCalibrateMusictButton = audio_reactive_ns.class_(
    "AudioReactiveCalibrateMusictButton", button.Button, cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_AUDIO_REACTIVE_ID): cv.use_id(AudioReactiveComponent),
        cv.Optional(CONF_RESET_AGC): button.button_schema(
            AudioReactiveResetAGCButton,
            icon="mdi:refresh",
            entity_category=ENTITY_CATEGORY_CONFIG,
        ),
        cv.Optional(CONF_CALIBRATE_QUIET): button.button_schema(
            AudioReactiveCalibrateQuietButton,
            icon="mdi:microphone-settings",
            entity_category=ENTITY_CATEGORY_CONFIG,
        ),
        cv.Optional(CONF_CALIBRATE_MUSIC): button.button_schema(
            AudioReactiveCalibrateMusictButton,
            icon="mdi:music-note-plus",
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

    if CONF_CALIBRATE_QUIET in config:
        btn = await button.new_button(config[CONF_CALIBRATE_QUIET])
        await cg.register_component(btn, config[CONF_CALIBRATE_QUIET])
        cg.add(btn.set_parent(parent))
        cg.add(parent.set_calibrate_quiet_button(btn))

    if CONF_CALIBRATE_MUSIC in config:
        btn = await button.new_button(config[CONF_CALIBRATE_MUSIC])
        await cg.register_component(btn, config[CONF_CALIBRATE_MUSIC])
        cg.add(btn.set_parent(parent))
        cg.add(parent.set_calibrate_music_button(btn))
