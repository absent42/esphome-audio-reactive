#pragma once

#include <cstdint>

#include "ring_buffer.h"
#include "fft_processor.h"
#include "band_aggregator.h"
#include "agc.h"
#include "onset_detector.h"
#include "silence_detector.h"
#include "dynamics_limiter.h"

#include "esphome/core/component.h"
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
class AudioReactiveCalibrateMusictButton;
class AudioReactiveSquelchNumber;
class AudioReactiveDetectionModeSelect;

/// Calibration state machine phases.
enum CalibrationState { CAL_IDLE, CAL_QUIET, CAL_MUSIC };

/// Persistent calibration data stored in NVS flash.
struct CalibrationStore {
    float squelch_threshold;    // From quiet cal: max_mid_high * 1.5
    float noise_floor_bass;     // Pre-scaled quiet max bass
    float noise_floor_mid;      // Pre-scaled quiet max mid
    float noise_floor_high;     // Pre-scaled quiet max high
    float noise_floor_amp;      // Pre-scaled quiet max amp
    float raw_scale;            // From music cal: 0.5 / avg_amplitude
    bool quiet_calibrated;
    bool music_calibrated;
};

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
class AudioReactiveCalibrateMusictButton : public button::Button, public Component {
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
    void set_squelch(float squelch) { squelch_ = squelch; }

    // Sensor setters (called from sensor.py / binary_sensor.py codegen)
    void set_bass_energy_sensor(sensor::Sensor *s) { bass_sensor_ = s; }
    void set_mid_energy_sensor(sensor::Sensor *s) { mid_sensor_ = s; }
    void set_high_energy_sensor(sensor::Sensor *s) { high_sensor_ = s; }
    void set_amplitude_sensor(sensor::Sensor *s) { amplitude_sensor_ = s; }
    void set_bpm_sensor(sensor::Sensor *s) { bpm_sensor_ = s; }

    // Binary sensor setters
    void set_onset_binary_sensor(binary_sensor::BinarySensor *s) { onset_sensor_ = s; }
    void set_silence_binary_sensor(binary_sensor::BinarySensor *s) { silence_sensor_ = s; }

    // Platform entity setters
    void set_beat_sensitivity_number(AudioReactiveBeatSensitivityNumber *n) { beat_sensitivity_number_ = n; }
    void set_squelch_number(AudioReactiveSquelchNumber *n) { squelch_number_ = n; }
    void set_mute_switch(AudioReactiveMicrophoneMuteSwitch *s) { mute_switch_ = s; }
    void set_reset_agc_button(AudioReactiveResetAGCButton *b) { reset_button_ = b; }
    void set_calibrate_quiet_button(AudioReactiveCalibrateQuietButton *b) { calibrate_quiet_button_ = b; }
    void set_calibrate_music_button(AudioReactiveCalibrateMusictButton *b) { calibrate_music_button_ = b; }
    void set_detection_mode_select(AudioReactiveDetectionModeSelect *s) { detection_mode_select_ = s; }

    void update_beat_sensitivity(int value);

    /// Mute/unmute the microphone. Stops audio processing and zeros outputs.
    void set_muted(bool muted);
    void toggle_mute();

    /// Reset AGC and onset detector state for re-calibration.
    void reset_agc();

    /// Start quiet room calibration (3 seconds, measures ambient noise).
    void start_quiet_calibration();
    /// Start music calibration (5 seconds, measures typical playback level).
    void start_music_calibration();

    friend class AudioReactiveBeatSensitivityNumber;
    friend class AudioReactiveMicrophoneMuteSwitch;
    friend class AudioReactiveResetAGCButton;
    friend class AudioReactiveCalibrateQuietButton;
    friend class AudioReactiveCalibrateMusictButton;
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
    binary_sensor::BinarySensor *onset_sensor_{nullptr};
    binary_sensor::BinarySensor *silence_sensor_{nullptr};

    // Platform entities
    AudioReactiveBeatSensitivityNumber *beat_sensitivity_number_{nullptr};
    AudioReactiveSquelchNumber *squelch_number_{nullptr};
    AudioReactiveMicrophoneMuteSwitch *mute_switch_{nullptr};
    AudioReactiveResetAGCButton *reset_button_{nullptr};
    AudioReactiveCalibrateQuietButton *calibrate_quiet_button_{nullptr};
    AudioReactiveCalibrateMusictButton *calibrate_music_button_{nullptr};
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
    uint32_t cal_sample_count_{0};
    float raw_scale_{1.0f / 20.0f};

    void finish_quiet_calibration();
    void finish_music_calibration();
    void apply_calibration();

    // DSP pipeline
    static constexpr size_t FFT_SIZE = 512;
    static constexpr float SAMPLE_RATE = 22050.0f;
    static constexpr size_t HOP_SIZE = 128;  // 75% overlap

    RingBuffer<float, 2048> ring_buffer_;  // 2048 for overlap headroom
    FFTProcessor<FFT_SIZE> *fft_{nullptr};
    BandAggregator band_agg_;
    AGC agc_bass_{AGC_NORMAL};
    AGC agc_mid_{AGC_NORMAL};
    AGC agc_high_{AGC_NORMAL};
    AGC agc_amp_{AGC_NORMAL};
    OnsetDetector *onset_det_{nullptr};
    SilenceDetector silence_det_;
    DynamicsLimiter limiter_;

    bool mic_started_{false};

    // FreeRTOS task for FFT on core 0
    TaskHandle_t fft_task_handle_{nullptr};
    static void fft_task_func(void *param);

    // Shared data between FFT task (core 0) and main loop (core 1)
    portMUX_TYPE fft_mux_ = portMUX_INITIALIZER_UNLOCKED;
    BandEnergies16 shared_energies_{};
    bool new_data_available_{false};

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

    /// Publish zero values to all sensors (used when muted or silent).
    void publish_zeros_();

    /// Apply asymmetric EMA smoothing: fast rise, slow fall.
    static float asymmetric_ema(float raw, float prev) {
        if (raw > prev) {
            return 0.75f * raw + 0.25f * prev;  // fast rise
        } else {
            return 0.17f * raw + 0.83f * prev;  // slow fall
        }
    }
};

}  // namespace audio_reactive
}  // namespace esphome
