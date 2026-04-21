#pragma once

#include <cstdint>

#include "esphome/core/defines.h"

#include "ring_buffer.h"
#include "fft_processor.h"
#include "band_aggregator.h"
#include "agc.h"
#include "onset_detector.h"
#include "beat_tracker.h"
#include "silence_detector.h"
#include "dynamics_limiter.h"
#include "spectral_whitening.h"
#include "calibration_migration.h"

#ifdef AUDIO_REACTIVE_PRO
// Pro-tier DSP block includes — added progressively in later chunks.
#include "mel_filterbank.h"
#include "musical_bands.h"
#include "superflux_onset.h"
#include "btrack.h"
#endif

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/preferences.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/number/number.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/button/button.h"
#include "esphome/components/select/select.h"
#include "esphome/components/microphone/microphone.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace audio_reactive {

// Forward declarations
class AudioReactiveComponent;
class AudioReactiveMicrophoneMuteSwitch;
class AudioReactiveResetAGCButton;
class AudioReactiveCalibrateQuietButton;
class AudioReactiveCalibrateMusicButton;
class AudioReactiveSquelchNumber;
class AudioReactiveDetectionModeSelect;

/// Calibration state machine phases.
enum CalibrationState { CAL_IDLE, CAL_QUIET, CAL_MUSIC };

/// Number entity for runtime beat sensitivity adjustment.
class AudioReactiveBeatSensitivityNumber : public number::Number, public Component {
 public:
    void set_parent(AudioReactiveComponent *parent) { parent_ = parent; }
    void setup() override;

 protected:
    void control(float value) override;
    AudioReactiveComponent *parent_{nullptr};
};

/// Switch entity for microphone mute control.
class AudioReactiveMicrophoneMuteSwitch : public switch_::Switch, public Component {
 public:
    void set_parent(AudioReactiveComponent *parent) { parent_ = parent; }
    void setup() override;

 protected:
    void write_state(bool state) override;
    AudioReactiveComponent *parent_{nullptr};
};

/// Button entity for resetting AGC calibration.
class AudioReactiveResetAGCButton : public button::Button, public Component {
 public:
    void set_parent(AudioReactiveComponent *parent) { parent_ = parent; }

 protected:
    void press_action() override;
    AudioReactiveComponent *parent_{nullptr};
};

/// Button entity for quiet room calibration.
class AudioReactiveCalibrateQuietButton : public button::Button, public Component {
 public:
    void set_parent(AudioReactiveComponent *parent) { parent_ = parent; }

 protected:
    void press_action() override;
    AudioReactiveComponent *parent_{nullptr};
};

/// Button entity for music level calibration.
class AudioReactiveCalibrateMusicButton : public button::Button, public Component {
 public:
    void set_parent(AudioReactiveComponent *parent) { parent_ = parent; }

 protected:
    void press_action() override;
    AudioReactiveComponent *parent_{nullptr};
};

/// Number entity for runtime squelch adjustment.
class AudioReactiveSquelchNumber : public number::Number, public Component {
 public:
    void set_parent(AudioReactiveComponent *parent) { parent_ = parent; }
    void setup() override;

 protected:
    void control(float value) override;
    AudioReactiveComponent *parent_{nullptr};
};

/// Select entity for onset detection mode.
class AudioReactiveDetectionModeSelect : public select::Select, public Component {
 public:
    void set_parent(AudioReactiveComponent *parent) { parent_ = parent; }
    void setup() override;

 protected:
    void control(const std::string &value) override;
    AudioReactiveComponent *parent_{nullptr};
};

class AudioReactiveComponent : public Component {
 public:
    void setup() override;
    void loop() override;
    void dump_config() override;
    float get_setup_priority() const override { return setup_priority::LATE; }

    void set_microphone(microphone::Microphone *mic) { mic_ = mic; }
    void set_update_interval(uint32_t interval_ms) { update_interval_ms_ = interval_ms; }
    void set_beat_sensitivity(int sensitivity) { beat_sensitivity_ = sensitivity; }
    void set_squelch(float squelch) { squelch_ = squelch; silence_det_.set_squelch(squelch); }
    void set_sample_rate(float rate) { sample_rate_ = rate; }
    void set_debug_logging(bool enabled) { debug_logging_ = enabled; }

    // Sensor setters (called from sensor.py / binary_sensor.py codegen)
    void set_bass_energy_sensor(sensor::Sensor *s) { bass_sensor_ = s; }
    void set_mid_energy_sensor(sensor::Sensor *s) { mid_sensor_ = s; }
    void set_high_energy_sensor(sensor::Sensor *s) { high_sensor_ = s; }
    void set_amplitude_sensor(sensor::Sensor *s) { amplitude_sensor_ = s; }
    void set_bpm_sensor(sensor::Sensor *s) { bpm_sensor_ = s; }
    void set_centroid_sensor(sensor::Sensor *s) { centroid_sensor_ = s; }
    void set_rolloff_sensor(sensor::Sensor *s) { rolloff_sensor_ = s; }
    void set_beat_confidence_sensor(sensor::Sensor *s) { beat_confidence_sensor_ = s; }
    void set_beat_phase_sensor(sensor::Sensor *s) { beat_phase_sensor_ = s; }
    void set_onset_strength_sensor(sensor::Sensor *s) { onset_strength_sensor_ = s; }

    // Binary sensor setters
    void set_onset_binary_sensor(binary_sensor::BinarySensor *s) { onset_sensor_ = s; }
    void set_silence_binary_sensor(binary_sensor::BinarySensor *s) { silence_sensor_ = s; }
#ifdef AUDIO_REACTIVE_PRO
    void set_calibration_stale_binary_sensor(binary_sensor::BinarySensor *s) { calibration_stale_sensor_ = s; }
    void set_beat_event_binary_sensor(binary_sensor::BinarySensor *s) { beat_event_sensor_ = s; }
    void set_sub_bass_energy_sensor(sensor::Sensor *s) { sub_bass_sensor_ = s; }
    void set_low_mid_energy_sensor(sensor::Sensor *s) { low_mid_sensor_ = s; }
    void set_upper_mid_energy_sensor(sensor::Sensor *s) { upper_mid_sensor_ = s; }
    void set_air_energy_sensor(sensor::Sensor *s) { air_sensor_ = s; }
#endif

    // Platform entity setters
    void set_beat_sensitivity_number(AudioReactiveBeatSensitivityNumber *n) { beat_sensitivity_number_ = n; }
    void set_squelch_number(AudioReactiveSquelchNumber *n) { squelch_number_ = n; }
    void set_mute_switch(AudioReactiveMicrophoneMuteSwitch *s) { mute_switch_ = s; }
    void set_reset_agc_button(AudioReactiveResetAGCButton *b) { reset_button_ = b; }
    void set_calibrate_quiet_button(AudioReactiveCalibrateQuietButton *b) { calibrate_quiet_button_ = b; }
    void set_calibrate_music_button(AudioReactiveCalibrateMusicButton *b) { calibrate_music_button_ = b; }
    void set_detection_mode_select(AudioReactiveDetectionModeSelect *s) { detection_mode_select_ = s; }

    void update_beat_sensitivity(int value);

    /// Mute/unmute the microphone. Stops audio processing and zeros outputs.
    void set_muted(bool muted);

    /// Reset AGC and onset detector state for re-calibration.
    void reset_agc();

    /// Start quiet room calibration (3 seconds, measures ambient noise).
    void start_quiet_calibration();
    /// Start music calibration (5 seconds, measures typical playback level).
    void start_music_calibration();

    // Automation trigger callback registration
    void add_on_mute_changed_callback(std::function<void()> &&callback) { on_mute_changed_callbacks_.add(std::move(callback)); }
    void add_on_quiet_calibration_started_callback(std::function<void()> &&callback) { on_quiet_calibration_started_callbacks_.add(std::move(callback)); }
    void add_on_quiet_calibration_complete_callback(std::function<void()> &&callback) { on_quiet_calibration_complete_callbacks_.add(std::move(callback)); }
    void add_on_music_calibration_started_callback(std::function<void()> &&callback) { on_music_calibration_started_callbacks_.add(std::move(callback)); }
    void add_on_music_calibration_complete_callback(std::function<void()> &&callback) { on_music_calibration_complete_callbacks_.add(std::move(callback)); }
    void add_on_silence_changed_callback(std::function<void()> &&callback) { on_silence_changed_callbacks_.add(std::move(callback)); }

    friend class AudioReactiveBeatSensitivityNumber;
    friend class AudioReactiveMicrophoneMuteSwitch;
    friend class AudioReactiveResetAGCButton;
    friend class AudioReactiveCalibrateQuietButton;
    friend class AudioReactiveCalibrateMusicButton;
    friend class AudioReactiveSquelchNumber;
    friend class AudioReactiveDetectionModeSelect;

 protected:
    microphone::Microphone *mic_{nullptr};
    uint32_t update_interval_ms_{50};
    int beat_sensitivity_{50};
    float squelch_{10.0f};
    bool muted_{false};

    // Sensors
    sensor::Sensor *bass_sensor_{nullptr};
    sensor::Sensor *mid_sensor_{nullptr};
    sensor::Sensor *high_sensor_{nullptr};
    sensor::Sensor *amplitude_sensor_{nullptr};
    sensor::Sensor *bpm_sensor_{nullptr};
    sensor::Sensor *centroid_sensor_{nullptr};
    sensor::Sensor *rolloff_sensor_{nullptr};
    sensor::Sensor *beat_confidence_sensor_{nullptr};
    sensor::Sensor *beat_phase_sensor_{nullptr};
    sensor::Sensor *onset_strength_sensor_{nullptr};
    binary_sensor::BinarySensor *onset_sensor_{nullptr};
    binary_sensor::BinarySensor *silence_sensor_{nullptr};

    // Platform entities
    AudioReactiveBeatSensitivityNumber *beat_sensitivity_number_{nullptr};
    AudioReactiveSquelchNumber *squelch_number_{nullptr};
    AudioReactiveMicrophoneMuteSwitch *mute_switch_{nullptr};
    AudioReactiveResetAGCButton *reset_button_{nullptr};
    AudioReactiveCalibrateQuietButton *calibrate_quiet_button_{nullptr};
    AudioReactiveCalibrateMusicButton *calibrate_music_button_{nullptr};
    AudioReactiveDetectionModeSelect *detection_mode_select_{nullptr};

    // Calibration persistence and state
    ESPPreferenceObject cal_pref_;
    CalibrationStore cal_store_{5.0f, 0.25f, 0.08f, 0.03f, 0.10f, 1.0f / 20.0f, false, false};
    CalibrationState cal_state_{CAL_IDLE};
    uint32_t cal_start_ms_{0};
    float cal_max_mid_high_{0};
    float cal_max_bass_{0};
    float cal_max_mid_{0};
    float cal_max_high_{0};
    float cal_max_amp_{0};
    float cal_sum_amp_{0};
    // Mean accumulators for per-band noise floor (quiet calibration)
    float cal_sum_bass_{0};
    float cal_sum_mid_{0};
    float cal_sum_high_{0};
    float cal_sum_amp_quiet_{0};
    uint32_t cal_quiet_count_{0};
    uint32_t cal_sample_count_{0};
    float raw_scale_{1.0f / 20.0f};

    void finish_quiet_calibration();
    void finish_music_calibration();
    void apply_calibration();
#ifdef AUDIO_REACTIVE_PRO
    /// Pro-tier calibration application.
    /// Chunk 2: delegates to basic-tier apply_calibration() as a fallback so the
    /// 4 basic-tier AGCs still receive noise floors during the pro-tier transition.
    /// Chunk 3: rewritten to wire musical_bands AGCs directly from cal_store_v2_.noise_floor[7].
    void apply_calibration_v2_();
#endif

    // DSP pipeline — configurable parameters
#ifdef AUDIO_REACTIVE_PRO
    static constexpr uint16_t FFT_SIZE = 2048;
    static constexpr uint16_t HOP_SIZE = 512;  // 75% overlap @ 86Hz frame rate (44.1kHz sample rate)
    static constexpr uint16_t RING_BUFFER_SIZE = 4096;
#else
    static constexpr uint16_t FFT_SIZE = 512;
    static constexpr uint16_t HOP_SIZE = FFT_SIZE / 4;  // 75% overlap
    static constexpr uint16_t RING_BUFFER_SIZE = 1024;
#endif
    float sample_rate_{22050.0f};
    bool debug_logging_{false};

    // Ring buffer: tier-gated size (2x FFT window)
    RingBuffer<float, RING_BUFFER_SIZE> ring_buffer_;
    FFTProcessor<FFT_SIZE> *fft_{nullptr};

    // Heap-allocated working buffers for FFT task (avoids stack overflow)
    float *fft_buffer_{nullptr};       // FFT_SIZE floats
    float *whitened_mags_{nullptr};    // FFT_SIZE/2 floats
    BandAggregator band_agg_;
    AGC agc_bass_{AGC_NORMAL};
    AGC agc_mid_{AGC_NORMAL};
    AGC agc_high_{AGC_NORMAL};
    AGC agc_amp_{AGC_NORMAL};
    OnsetDetector *onset_det_{nullptr};
    BeatTracker *beat_tracker_{nullptr};
    SilenceDetector silence_det_;
    DynamicsLimiter limiter_;
    SpectralWhitening<256> whitening_{0.0f};  // Properly initialized in setup()

    bool mic_started_{false};

    // FreeRTOS task for FFT on core 0
    TaskHandle_t fft_task_handle_{nullptr};
    static void fft_task_func(void *param);

    // Shared data between FFT task (core 0) and main loop (core 1)
    // Double-buffer with atomic index swap eliminates spinlock in hot path.
    struct SharedFrame {
        BandEnergies16 energies{};
        float complex_onset{0.0f};
#ifdef AUDIO_REACTIVE_PRO
        // Raw (pre-log) mel-band energies. 32 matches N_MEL below (kept as a literal
        // here because N_MEL is declared later in the pro-tier member block).
        float mel_frame[32]{};
        float superflux_strength{0.0f};
        bool superflux_event{false};
        float btrack_bpm{120.0f};
        float btrack_phase{0.0f};
        float btrack_confidence{0.0f};
        bool btrack_event{false};
        uint32_t frame_id{0};
#endif
    };
    SharedFrame shared_frames_[2]{};
    volatile int shared_write_idx_{0};   // FFT task writes to [write ^ 1], then flips
    volatile bool new_data_available_{false};

    // Complex domain onset: previous-frame state (heap members, not FFT task stack)
#ifndef AUDIO_REACTIVE_PRO
    float prev_phases_[256]{};
    float prev_magnitudes_[256]{};
    bool has_prev_frame_{false};
#endif

    // Smoothed values for asymmetric EMA
    float smooth_bass_{0.0f};
    float smooth_mid_{0.0f};
    float smooth_high_{0.0f};
    float smooth_amp_{0.0f};

    // Timing
    uint32_t last_process_ms_{0};
    uint32_t last_bpm_publish_ms_{0};
    static constexpr uint32_t BPM_PUBLISH_INTERVAL_MS = 3000;

    // Onset pulse timing
    uint32_t onset_on_ms_{0};
    static constexpr uint32_t ONSET_PULSE_DURATION_MS = 80;

    // Previous silence state for edge detection
    bool prev_silence_{false};

    // Automation trigger callbacks
    CallbackManager<void()> on_mute_changed_callbacks_;
    CallbackManager<void()> on_quiet_calibration_started_callbacks_;
    CallbackManager<void()> on_quiet_calibration_complete_callbacks_;
    CallbackManager<void()> on_music_calibration_started_callbacks_;
    CallbackManager<void()> on_music_calibration_complete_callbacks_;
    CallbackManager<void()> on_silence_changed_callbacks_;

    /// Publish zero values to all sensors (used when muted or silent).
    void publish_zeros_();
    /// Debug logging block (runs every 2s when debug_logging_ is true).
    void process_debug_logging_(uint32_t now, const BandEnergies16 &energies);
    /// Accumulate calibration samples during quiet/music calibration.
    void accumulate_calibration_(uint32_t now, const BandEnergies16 &energies);
    /// Publish all sensor values from the current DSP frame.
    void publish_sensor_values_(uint32_t now, const BandEnergies16 &energies, const OnsetDetector::OnsetResult &onset_result);

    // Asymmetric EMA coefficients: fast attack (~36ms at 20Hz), slow decay (~270ms)
    static constexpr float EMA_FAST_RISE = 0.75f;
    static constexpr float EMA_SLOW_FALL = 0.17f;

    /// Apply asymmetric EMA smoothing: fast rise, slow fall.
    static float asymmetric_ema(float raw, float prev) {
        float alpha = (raw > prev) ? EMA_FAST_RISE : EMA_SLOW_FALL;
        return alpha * raw + (1.0f - alpha) * prev;
    }

#ifdef AUDIO_REACTIVE_PRO
    // Pro-tier calibration (V2 format, per-musical-band noise floors)
    ESPPreferenceObject cal_pref_v2_;
    CalibrationStoreV2 cal_store_v2_{2, {0, 0, 0}, 5.0f, {0, 0, 0, 0, 0, 0, 0}, 1.0f / 20.0f, false, false, {0, 0}};
    bool cal_stale_{false};  // true if loaded from V1 migration or uncalibrated

    // Diagnostic binary sensor - exposed via binary_sensor.py
    binary_sensor::BinarySensor *calibration_stale_sensor_{nullptr};

    // BTrack beat event binary sensor (published on beat boundaries, 30ms pulse).
    binary_sensor::BinarySensor *beat_event_sensor_{nullptr};
    uint32_t beat_event_off_at_ms_{0};

    // Frame-id counter: FFT task increments on every write so the main loop
    // can consume each frame at most once (prevents double-publish of beat_event
    // given the main-loop ~50Hz / FFT-task ~86Hz cadence mismatch).
    uint32_t next_frame_id_{0};
    uint32_t last_consumed_frame_id_{0};

    static constexpr uint8_t N_MEL = 32;

    // Pro-tier DSP blocks (owned; setup() initializes)
    MelFilterbank<N_MEL, FFT_SIZE> mel_fb_;
    MusicalBands musical_bands_;
    SuperFluxOnset<N_MEL> superflux_;
    BTrack btrack_;
    // Per-frame state published to main loop via SharedFrame

    // Pre-allocated working buffers (heap/PSRAM via setup(), not FFT-task stack).
    // fft_task_func is a 6144-byte-stack pinned task; large per-frame buffers MUST
    // be class members or heap-allocated, not stack-locals inside the task body.
    float *mags_sq_{nullptr};        // FFT_SIZE/2 floats — squared magnitudes for mel input

    // Pro-tier 7-band energies (written by main loop after AGC+EMA)
    float musical_band_energies_[MusicalBands::kNumBands]{};

    // Per-musical-band sensor pointers (published from main loop)
    sensor::Sensor *sub_bass_sensor_{nullptr};
    sensor::Sensor *low_mid_sensor_{nullptr};
    sensor::Sensor *upper_mid_sensor_{nullptr};
    sensor::Sensor *air_sensor_{nullptr};
#endif  // AUDIO_REACTIVE_PRO
};

// Automation triggers (macro-generated — each class is structurally identical)
#define AUDIO_REACTIVE_TRIGGER(ClassName, callback_method)                   \
class ClassName : public Trigger<> {                                         \
 public:                                                                     \
    explicit ClassName(AudioReactiveComponent *parent) {                     \
        parent->callback_method([this]() { this->trigger(); });             \
    }                                                                        \
};

AUDIO_REACTIVE_TRIGGER(AudioReactiveMuteChangedTrigger, add_on_mute_changed_callback)
AUDIO_REACTIVE_TRIGGER(AudioReactiveQuietCalibrationStartedTrigger, add_on_quiet_calibration_started_callback)
AUDIO_REACTIVE_TRIGGER(AudioReactiveQuietCalibrationCompleteTrigger, add_on_quiet_calibration_complete_callback)
AUDIO_REACTIVE_TRIGGER(AudioReactiveMusicCalibrationStartedTrigger, add_on_music_calibration_started_callback)
AUDIO_REACTIVE_TRIGGER(AudioReactiveMusicCalibrationCompleteTrigger, add_on_music_calibration_complete_callback)
AUDIO_REACTIVE_TRIGGER(AudioReactiveSilenceChangedTrigger, add_on_silence_changed_callback)

#undef AUDIO_REACTIVE_TRIGGER

}  // namespace audio_reactive
}  // namespace esphome
