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

CONF_AUDIO_REACTIVE_ID = "audio_reactive_id"
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

CONF_DSP_TIER = "dsp_tier"

DSP_TIER_AUTO = "auto"
DSP_TIER_BASIC = "basic"
DSP_TIER_PRO = "pro"
DSP_TIERS = [DSP_TIER_AUTO, DSP_TIER_BASIC, DSP_TIER_PRO]

# Tier-driven sample rates (must match the upstream microphone component's sample_rate)
DSP_RATE_BASIC = 22050
DSP_RATE_PRO = 44100

from esphome.components import microphone

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(AudioReactiveComponent),
        cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
        cv.Optional(CONF_UPDATE_INTERVAL, default="50ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_BEAT_SENSITIVITY, default=50): cv.int_range(min=1, max=100),
        cv.Optional(CONF_SQUELCH, default=10): cv.int_range(min=0, max=100),
        cv.Optional(CONF_DSP_TIER, default=DSP_TIER_AUTO): cv.one_of(*DSP_TIERS, lower=True),
        cv.Optional(CONF_FFT_SIZE, default=512): cv.one_of(512, int=True),  # Fixed at 512 (FFTProcessor template constraint)
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


def _find_microphone_sample_rate(config):
    """Find the declared sample_rate on the referenced microphone component.

    At codegen time, CORE.config is the fully-loaded top-level YAML dict.
    The microphone is declared as a top-level list (`microphone:` or
    platform-specific blocks like `i2s_audio:` with a `microphone` entry).
    We search for the entry whose id matches this component's configured mic.

    ESPHome enforces id uniqueness across the whole config, so the first
    id-matching entry found is the correct one.

    Returns: int sample rate, or None if it could not be determined.
    """
    from esphome.core import CORE

    target_id = str(config[CONF_MICROPHONE])

    def _scan(node):
        """Recursively scan a config node for a microphone entry whose id matches."""
        if isinstance(node, dict):
            node_id = node.get("id")
            if node_id is not None and str(node_id) == target_id:
                return node.get("sample_rate")
            for v in node.values():
                r = _scan(v)
                if r is not None:
                    return r
        elif isinstance(node, list):
            for item in node:
                r = _scan(item)
                if r is not None:
                    return r
        return None

    return _scan(CORE.config)


def resolve_tier_from_core():
    """Tier-only resolution that does NOT require CONF_MICROPHONE in config.

    Safe to call from sibling codegen modules (binary_sensor.py, sensor.py)
    that need to know the tier but don't hold a reference to the audio_reactive
    component's own config (which has the mic id).

    Reads the top-level `audio_reactive:` YAML block from CORE.config and
    returns the tier string. Performs SoC/PSRAM validation but skips mic-rate
    validation (that happens once in resolve_tier() during the parent's
    to_code() run).

    Returns: 'basic' or 'pro'. Raises cv.Invalid if 'pro' is requested but
    hardware can't support it.
    """
    from esphome.core import CORE
    from esphome.components.esp32 import get_esp32_variant

    ar_config = CORE.config.get("audio_reactive") or {}
    requested = ar_config.get(CONF_DSP_TIER, DSP_TIER_AUTO)
    try:
        variant = get_esp32_variant()
    except Exception:
        variant = None
    is_s3 = variant == "ESP32S3"
    has_psram = CORE.config.get("psram") is not None

    if requested == DSP_TIER_AUTO:
        return DSP_TIER_PRO if (is_s3 and has_psram) else DSP_TIER_BASIC
    if requested == DSP_TIER_PRO and not (is_s3 and has_psram):
        raise cv.Invalid(
            "dsp_tier: pro requires an ESP32-S3 board with a top-level "
            "'psram:' YAML block."
        )
    return requested


def resolve_tier(config):
    """Resolve dsp_tier and validate consistency with SoC, PSRAM, and mic rate.

    Returns: tuple (resolved_tier: str, expected_rate: int).
    Raises cv.Invalid if 'pro' is requested but hardware cannot support it,
    or if the upstream microphone's declared sample_rate does not match
    the resolved tier's expected rate.

    Must be called from to_code() (requires CORE.config fully populated).
    """
    from esphome.core import CORE
    from esphome.components.esp32 import get_esp32_variant

    requested = config[CONF_DSP_TIER]
    try:
        variant = get_esp32_variant()
    except Exception:
        variant = None
    is_s3 = variant == "ESP32S3"
    has_psram = CORE.config.get("psram") is not None

    if requested == DSP_TIER_AUTO:
        resolved = DSP_TIER_PRO if (is_s3 and has_psram) else DSP_TIER_BASIC
    elif requested == DSP_TIER_PRO and not (is_s3 and has_psram):
        raise cv.Invalid(
            "dsp_tier: pro requires an ESP32-S3 board with a top-level "
            "'psram:' YAML block. Your configuration does not meet this "
            "requirement. Use dsp_tier: basic or auto."
        )
    else:
        resolved = requested

    expected_rate = DSP_RATE_PRO if resolved == DSP_TIER_PRO else DSP_RATE_BASIC

    # Validate upstream microphone's declared sample_rate matches the tier.
    mic_rate = _find_microphone_sample_rate(config)
    if mic_rate is None:
        # The mic platform may not expose sample_rate in its config (rare).
        import logging
        logging.getLogger(__name__).warning(
            "audio_reactive: could not determine microphone sample_rate from "
            "YAML. Expected %d Hz for dsp_tier '%s'. Verify manually.",
            expected_rate, resolved,
        )
    elif int(mic_rate) != expected_rate:
        raise cv.Invalid(
            f"audio_reactive: microphone sample_rate is {mic_rate} Hz but "
            f"dsp_tier '{resolved}' requires {expected_rate} Hz. Update the "
            f"'sample_rate:' value under your microphone component (e.g., "
            f"i2s_audio) to {expected_rate}, or change dsp_tier."
        )

    return resolved, expected_rate


async def to_code(config):
    cg.add_library("arduinoFFT", "2.0.4")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    mic = await cg.get_variable(config[CONF_MICROPHONE])
    cg.add(var.set_microphone(mic))
    cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL]))
    cg.add(var.set_beat_sensitivity(config[CONF_BEAT_SENSITIVITY]))
    cg.add(var.set_squelch(config[CONF_SQUELCH]))
    cg.add(var.set_debug_logging(config[CONF_DEBUG_LOGGING]))

    tier, expected_rate = resolve_tier(config)
    if tier == DSP_TIER_PRO:
        cg.add_define("AUDIO_REACTIVE_PRO")
    cg.add(var.set_sample_rate(float(expected_rate)))

    # Automation triggers
    for trigger_key in [
        CONF_ON_MUTE_CHANGED,
        CONF_ON_QUIET_CALIBRATION_STARTED,
        CONF_ON_QUIET_CALIBRATION_COMPLETE,
        CONF_ON_MUSIC_CALIBRATION_STARTED,
        CONF_ON_MUSIC_CALIBRATION_COMPLETE,
        CONF_ON_SILENCE_CHANGED,
    ]:
        for conf in config.get(trigger_key, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
            await automation.build_automation(trigger, [], conf)
