"""Number platform for audio_reactive component."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import AudioReactiveComponent, audio_reactive_ns

CONF_AUDIO_REACTIVE_ID = "audio_reactive_id"
CONF_BEAT_SENSITIVITY = "beat_sensitivity"
CONF_SQUELCH = "squelch"

AudioReactiveBeatSensitivityNumber = audio_reactive_ns.class_(
    "AudioReactiveBeatSensitivityNumber", number.Number, cg.Component
)

AudioReactiveSquelchNumber = audio_reactive_ns.class_(
    "AudioReactiveSquelchNumber", number.Number, cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_AUDIO_REACTIVE_ID): cv.use_id(AudioReactiveComponent),
        cv.Optional(CONF_BEAT_SENSITIVITY): number.number_schema(
            AudioReactiveBeatSensitivityNumber,
            icon="mdi:sine-wave",
            entity_category=ENTITY_CATEGORY_CONFIG,
        ),
        cv.Optional(CONF_SQUELCH): number.number_schema(
            AudioReactiveSquelchNumber,
            icon="mdi:volume-off",
            entity_category=ENTITY_CATEGORY_CONFIG,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_AUDIO_REACTIVE_ID])

    if CONF_BEAT_SENSITIVITY in config:
        num = await number.new_number(
            config[CONF_BEAT_SENSITIVITY],
            min_value=1,
            max_value=100,
            step=1,
        )
        await cg.register_component(num, config[CONF_BEAT_SENSITIVITY])
        cg.add(num.set_parent(parent))
        cg.add(parent.set_beat_sensitivity_number(num))

    if CONF_SQUELCH in config:
        num = await number.new_number(
            config[CONF_SQUELCH],
            min_value=0,
            max_value=100,
            step=1,
        )
        await cg.register_component(num, config[CONF_SQUELCH])
        cg.add(num.set_parent(parent))
        cg.add(parent.set_squelch_number(num))
