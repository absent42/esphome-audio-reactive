"""Sensor platform for audio_reactive component."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ID,
    STATE_CLASS_MEASUREMENT,
)

from . import AudioReactiveComponent, CONF_AUDIO_REACTIVE_ID
CONF_BASS_ENERGY = "bass_energy"
CONF_MID_ENERGY = "mid_energy"
CONF_HIGH_ENERGY = "high_energy"
CONF_AMPLITUDE = "amplitude"
CONF_BPM = "bpm"
CONF_CENTROID = "centroid"
CONF_ROLLOFF = "rolloff"
CONF_BEAT_CONFIDENCE = "beat_confidence"
CONF_BEAT_PHASE = "beat_phase"
CONF_ONSET_STRENGTH = "onset_strength"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_AUDIO_REACTIVE_ID): cv.use_id(AudioReactiveComponent),
        cv.Optional(CONF_BASS_ENERGY): sensor.sensor_schema(
            accuracy_decimals=3,
            icon="mdi:equalizer",
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_MID_ENERGY): sensor.sensor_schema(
            accuracy_decimals=3,
            icon="mdi:equalizer",
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_HIGH_ENERGY): sensor.sensor_schema(
            accuracy_decimals=3,
            icon="mdi:equalizer",
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_AMPLITUDE): sensor.sensor_schema(
            accuracy_decimals=3,
            icon="mdi:volume-high",
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_BPM): sensor.sensor_schema(
            accuracy_decimals=0,
            icon="mdi:metronome",
            unit_of_measurement="BPM",
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_CENTROID): sensor.sensor_schema(
            accuracy_decimals=3,
            icon="mdi:chart-bell-curve",
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_ROLLOFF): sensor.sensor_schema(
            accuracy_decimals=3,
            icon="mdi:chart-bell-curve-cumulative",
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_BEAT_CONFIDENCE): sensor.sensor_schema(
            accuracy_decimals=2,
            icon="mdi:metronome-tick",
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_BEAT_PHASE): sensor.sensor_schema(
            accuracy_decimals=3,
            icon="mdi:sine-wave",
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_ONSET_STRENGTH): sensor.sensor_schema(
            accuracy_decimals=3,
            icon="mdi:flash",
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_AUDIO_REACTIVE_ID])

    for key, setter in [
        (CONF_BASS_ENERGY, "set_bass_energy_sensor"),
        (CONF_MID_ENERGY, "set_mid_energy_sensor"),
        (CONF_HIGH_ENERGY, "set_high_energy_sensor"),
        (CONF_AMPLITUDE, "set_amplitude_sensor"),
        (CONF_BPM, "set_bpm_sensor"),
        (CONF_CENTROID, "set_centroid_sensor"),
        (CONF_ROLLOFF, "set_rolloff_sensor"),
        (CONF_BEAT_CONFIDENCE, "set_beat_confidence_sensor"),
        (CONF_BEAT_PHASE, "set_beat_phase_sensor"),
        (CONF_ONSET_STRENGTH, "set_onset_strength_sensor"),
    ]:
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(parent, setter)(sens))
