"""Select platform for audio_reactive component."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import AudioReactiveComponent, audio_reactive_ns

CONF_AUDIO_REACTIVE_ID = "audio_reactive_id"
CONF_DETECTION_MODE = "detection_mode"

AudioReactiveDetectionModeSelect = audio_reactive_ns.class_(
    "AudioReactiveDetectionModeSelect", select.Select, cg.Component
)

DETECTION_MODE_OPTIONS = ["spectral_flux", "bass_energy"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_AUDIO_REACTIVE_ID): cv.use_id(AudioReactiveComponent),
        cv.Optional(CONF_DETECTION_MODE): select.select_schema(
            AudioReactiveDetectionModeSelect,
            icon="mdi:waveform",
            entity_category=ENTITY_CATEGORY_CONFIG,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_AUDIO_REACTIVE_ID])

    if CONF_DETECTION_MODE in config:
        sel = await select.new_select(
            config[CONF_DETECTION_MODE],
            options=DETECTION_MODE_OPTIONS,
        )
        await cg.register_component(sel, config[CONF_DETECTION_MODE])
        cg.add(sel.set_parent(parent))
        cg.add(parent.set_detection_mode_select(sel))
