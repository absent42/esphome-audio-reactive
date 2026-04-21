"""Sensor platform for audio_reactive component."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ID,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
)

from . import (
    AudioReactiveComponent,
    CONF_AUDIO_REACTIVE_ID,
    resolve_tier_from_core,
    DSP_TIER_PRO,
)

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

# Pro-tier only: additional perceptual musical bands (sub-bass, low-mid, upper-mid, air).
# bass/mid/high remain available on both tiers (basic = band-aggregator output,
# pro = musical-bands output from mel filterbank).
CONF_SUB_BASS_ENERGY = "sub_bass_energy"
CONF_LOW_MID_ENERGY = "low_mid_energy"
CONF_UPPER_MID_ENERGY = "upper_mid_energy"
CONF_AIR_ENERGY = "air_energy"

# Debug sensors (both tiers): mean/peak FFT task cycle time in microseconds.
# Published at 1Hz from loop(), measured inside fft_task_func excluding wait time.
CONF_FFT_TASK_CYCLE_MEAN_US = "fft_task_cycle_mean_us"
CONF_FFT_TASK_CYCLE_PEAK_US = "fft_task_cycle_peak_us"

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
        cv.Optional(CONF_SUB_BASS_ENERGY): sensor.sensor_schema(
            accuracy_decimals=3,
            icon="mdi:equalizer",
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_LOW_MID_ENERGY): sensor.sensor_schema(
            accuracy_decimals=3,
            icon="mdi:equalizer",
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_UPPER_MID_ENERGY): sensor.sensor_schema(
            accuracy_decimals=3,
            icon="mdi:equalizer",
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_AIR_ENERGY): sensor.sensor_schema(
            accuracy_decimals=3,
            icon="mdi:equalizer",
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_FFT_TASK_CYCLE_MEAN_US): sensor.sensor_schema(
            unit_of_measurement="µs",
            accuracy_decimals=1,
            icon="mdi:timer-outline",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_FFT_TASK_CYCLE_PEAK_US): sensor.sensor_schema(
            unit_of_measurement="µs",
            accuracy_decimals=0,
            icon="mdi:timer-alert-outline",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    }
)


# Pro-only sensor keys — gated in to_code() via resolve_tier_from_core().
_PRO_ONLY_KEYS = {
    CONF_SUB_BASS_ENERGY,
    CONF_LOW_MID_ENERGY,
    CONF_UPPER_MID_ENERGY,
    CONF_AIR_ENERGY,
}


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
        (CONF_SUB_BASS_ENERGY, "set_sub_bass_energy_sensor"),
        (CONF_LOW_MID_ENERGY, "set_low_mid_energy_sensor"),
        (CONF_UPPER_MID_ENERGY, "set_upper_mid_energy_sensor"),
        (CONF_AIR_ENERGY, "set_air_energy_sensor"),
        (CONF_FFT_TASK_CYCLE_MEAN_US, "set_fft_task_cycle_mean_sensor"),
        (CONF_FFT_TASK_CYCLE_PEAK_US, "set_fft_task_cycle_peak_sensor"),
    ]:
        if key in config:
            if key in _PRO_ONLY_KEYS:
                tier = resolve_tier_from_core()
                if tier != DSP_TIER_PRO:
                    raise cv.Invalid(
                        f"sensor.{key} requires dsp_tier: pro. "
                        f"This sensor is only available on ESP32-S3 + PSRAM boards."
                    )
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(parent, setter)(sens))
