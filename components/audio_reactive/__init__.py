"""ESPHome audio_reactive component."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@absent42"]
DEPENDENCIES = ["microphone"]
AUTO_LOAD = ["number", "select", "switch", "button"]

audio_reactive_ns = cg.esphome_ns.namespace("audio_reactive")
AudioReactiveComponent = audio_reactive_ns.class_(
    "AudioReactiveComponent", cg.Component
)

# Automation trigger classes
AudioReactiveMuteChangedTrigger = audio_reactive_ns.class_(
    "AudioReactiveMuteChangedTrigger", automation.Trigger.template()
)
AudioReactiveQuietCalibrationStartedTrigger = audio_reactive_ns.class_(
    "AudioReactiveQuietCalibrationStartedTrigger", automation.Trigger.template()
)
AudioReactiveQuietCalibrationCompleteTrigger = audio_reactive_ns.class_(
    "AudioReactiveQuietCalibrationCompleteTrigger", automation.Trigger.template()
)
AudioReactiveMusicCalibrationStartedTrigger = audio_reactive_ns.class_(
    "AudioReactiveMusicCalibrationStartedTrigger", automation.Trigger.template()
)
AudioReactiveMusicCalibrationCompleteTrigger = audio_reactive_ns.class_(
    "AudioReactiveMusicCalibrationCompleteTrigger", automation.Trigger.template()
)
AudioReactiveSilenceChangedTrigger = audio_reactive_ns.class_(
    "AudioReactiveSilenceChangedTrigger", automation.Trigger.template()
)

CONF_MICROPHONE = "microphone"
CONF_UPDATE_INTERVAL = "update_interval"
CONF_BEAT_SENSITIVITY = "beat_sensitivity"
CONF_SQUELCH = "squelch"
CONF_SAMPLE_RATE = "sample_rate"
CONF_FFT_SIZE = "fft_size"
CONF_DEBUG_LOGGING = "debug_logging"
CONF_ON_MUTE_CHANGED = "on_mute_changed"
CONF_ON_QUIET_CALIBRATION_STARTED = "on_quiet_calibration_started"
CONF_ON_QUIET_CALIBRATION_COMPLETE = "on_quiet_calibration_complete"
CONF_ON_MUSIC_CALIBRATION_STARTED = "on_music_calibration_started"
CONF_ON_MUSIC_CALIBRATION_COMPLETE = "on_music_calibration_complete"
CONF_ON_SILENCE_CHANGED = "on_silence_changed"

from esphome.components import microphone

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(AudioReactiveComponent),
        cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
        cv.Optional(CONF_UPDATE_INTERVAL, default="50ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_BEAT_SENSITIVITY, default=50): cv.int_range(min=1, max=100),
        cv.Optional(CONF_SQUELCH, default=10): cv.int_range(min=0, max=100),
        cv.Optional(CONF_SAMPLE_RATE, default=22050): cv.int_range(min=8000, max=96000),
        cv.Optional(CONF_FFT_SIZE, default=512): cv.one_of(256, 512, int=True),  # 1024 not yet supported (FFTProcessor template constraint)
        cv.Optional(CONF_DEBUG_LOGGING, default=False): cv.boolean,
        cv.Optional(CONF_ON_MUTE_CHANGED): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(AudioReactiveMuteChangedTrigger)}
        ),
        cv.Optional(CONF_ON_QUIET_CALIBRATION_STARTED): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(AudioReactiveQuietCalibrationStartedTrigger)}
        ),
        cv.Optional(CONF_ON_QUIET_CALIBRATION_COMPLETE): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(AudioReactiveQuietCalibrationCompleteTrigger)}
        ),
        cv.Optional(CONF_ON_MUSIC_CALIBRATION_STARTED): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(AudioReactiveMusicCalibrationStartedTrigger)}
        ),
        cv.Optional(CONF_ON_MUSIC_CALIBRATION_COMPLETE): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(AudioReactiveMusicCalibrationCompleteTrigger)}
        ),
        cv.Optional(CONF_ON_SILENCE_CHANGED): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(AudioReactiveSilenceChangedTrigger)}
        ),
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
    cg.add(var.set_squelch(config[CONF_SQUELCH]))
    cg.add(var.set_sample_rate(config[CONF_SAMPLE_RATE]))
    cg.add(var.set_fft_size(config[CONF_FFT_SIZE]))
    cg.add(var.set_debug_logging(config[CONF_DEBUG_LOGGING]))

    # Automation triggers
    for conf in config.get(CONF_ON_MUTE_CHANGED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_QUIET_CALIBRATION_STARTED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_QUIET_CALIBRATION_COMPLETE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_MUSIC_CALIBRATION_STARTED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_MUSIC_CALIBRATION_COMPLETE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_SILENCE_CHANGED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
