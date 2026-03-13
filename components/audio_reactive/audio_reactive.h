#pragma once

#include "agc.h"
#include "band_aggregator.h"
#include "beat_detector.h"
#include "fft_processor.h"

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/microphone/microphone.h"

namespace esphome {
namespace audio_reactive {

class AudioReactiveComponent : public Component {
 public:
    void setup() override;
    void loop() override;
    void dump_config() override;
    float get_setup_priority() const override { return setup_priority::LATE; }

    void set_microphone(microphone::Microphone *mic) { mic_ = mic; }
    void set_update_interval(uint32_t interval_ms) { update_interval_ms_ = interval_ms; }
    void set_beat_sensitivity(int sensitivity) { beat_sensitivity_ = sensitivity; }

    // Sensor setters (called from sensor.py / binary_sensor.py codegen)
    void set_bass_energy_sensor(sensor::Sensor *s) { bass_sensor_ = s; }
    void set_mid_energy_sensor(sensor::Sensor *s) { mid_sensor_ = s; }
    void set_high_energy_sensor(sensor::Sensor *s) { high_sensor_ = s; }
    void set_amplitude_sensor(sensor::Sensor *s) { amplitude_sensor_ = s; }
    void set_bpm_sensor(sensor::Sensor *s) { bpm_sensor_ = s; }
    void set_beat_binary_sensor(binary_sensor::BinarySensor *s) { beat_sensor_ = s; }

    /// Reset AGC and beat detector state for re-calibration.
    void reset_agc();

 protected:
    microphone::Microphone *mic_{nullptr};
    uint32_t update_interval_ms_{50};
    int beat_sensitivity_{50};

    // Sensors
    sensor::Sensor *bass_sensor_{nullptr};
    sensor::Sensor *mid_sensor_{nullptr};
    sensor::Sensor *high_sensor_{nullptr};
    sensor::Sensor *amplitude_sensor_{nullptr};
    sensor::Sensor *bpm_sensor_{nullptr};
    binary_sensor::BinarySensor *beat_sensor_{nullptr};

    // DSP pipeline — FFT size hardcoded to 512 (spec recommendation:
    // 512 samples at 10kHz gives ~20Hz resolution, sufficient for bass band)
    static constexpr size_t FFT_SIZE = 512;
    FFTProcessor<FFT_SIZE> *fft_{nullptr};
    BandAggregator *band_agg_{nullptr};
    AGC *agc_bass_{nullptr};
    AGC *agc_mid_{nullptr};
    AGC *agc_high_{nullptr};
    AGC *agc_amp_{nullptr};
    BeatDetector *beat_det_{nullptr};

    // Audio buffer (written by I2S callback task, read by main loop)
    float *sample_buffer_{nullptr};
    volatile size_t samples_collected_{0};
    volatile bool processing_{false};
    bool mic_started_{false};

    // Timing
    uint32_t last_process_ms_{0};
    uint32_t last_bpm_publish_ms_{0};
    static constexpr uint32_t BPM_PUBLISH_INTERVAL_MS = 3000;

    // Beat pulse timing
    uint32_t beat_on_ms_{0};
    static constexpr uint32_t BEAT_PULSE_DURATION_MS = 80;

    void process_audio_();
};

}  // namespace audio_reactive
}  // namespace esphome
