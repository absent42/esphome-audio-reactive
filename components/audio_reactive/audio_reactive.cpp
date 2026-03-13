#include "audio_reactive.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace audio_reactive {

static const char *const TAG = "audio_reactive";

void AudioReactiveComponent::setup() {
    ESP_LOGCONFIG(TAG, "Setting up AudioReactive...");

    // Allocate DSP pipeline
    float sample_rate = 16000.0f;  // I2S configured for 16kHz
    fft_ = new FFTProcessor<FFT_SIZE>(sample_rate);
    float freq_res = sample_rate / static_cast<float>(FFT_SIZE);
    band_agg_ = new BandAggregator(freq_res);
    agc_bass_ = new AGC(100);  // ~5s at 20 updates/s
    agc_mid_ = new AGC(100);
    agc_high_ = new AGC(100);
    agc_amp_ = new AGC(100);
    beat_det_ = new BeatDetector(
        beat_sensitivity_, 20, 150
    );

    // Allocate sample buffer
    sample_buffer_ = new float[FFT_SIZE];
    samples_collected_ = 0;

    // Register callback for incoming audio data (called from I2S task)
    if (mic_ != nullptr) {
        mic_->add_data_callback([this](const std::vector<uint8_t> &data) {
            if (processing_) return;  // Skip while main loop is reading the buffer
            const int16_t *samples = reinterpret_cast<const int16_t *>(data.data());
            size_t sample_count = data.size() / sizeof(int16_t);
            for (size_t i = 0; i < sample_count && samples_collected_ < FFT_SIZE; i++) {
                sample_buffer_[samples_collected_++] =
                    static_cast<float>(samples[i]) / 32768.0f;
            }
        });
    } else {
        ESP_LOGW(TAG, "No microphone assigned!");
    }

    ESP_LOGI(TAG, "Initialized (FFT=%u, interval=%ums, sensitivity=%d)",
             FFT_SIZE, update_interval_ms_, beat_sensitivity_);
}

void AudioReactiveComponent::loop() {
    // Start microphone on first loop iteration (after all components are set up)
    if (!mic_started_ && mic_ != nullptr) {
        mic_->start();
        mic_started_ = true;
        ESP_LOGI(TAG, "Microphone started");
    }

    uint32_t now = millis();

    // Process when we have enough samples and interval has elapsed
    if (samples_collected_ >= FFT_SIZE &&
        (now - last_process_ms_) >= update_interval_ms_) {
        processing_ = true;   // Pause callback writes
        process_audio_();
        samples_collected_ = 0;
        processing_ = false;  // Resume callback writes
        last_process_ms_ = now;
    }

    // Turn off beat binary sensor after pulse duration
    if (beat_on_ms_ > 0 && (now - beat_on_ms_) >= BEAT_PULSE_DURATION_MS) {
        if (beat_sensor_ != nullptr) {
            beat_sensor_->publish_state(false);
        }
        beat_on_ms_ = 0;
    }
}

void AudioReactiveComponent::process_audio_() {
    uint32_t now = millis();

    // FFT
    fft_->process(sample_buffer_);
    const float *magnitudes = fft_->magnitudes();

    // Band aggregation
    auto bands = band_agg_->aggregate(magnitudes, fft_->bin_count());

    // AGC normalization — each band uses its own AGC instance
    agc_bass_->update(bands.bass);
    agc_mid_->update(bands.mid);
    agc_high_->update(bands.high);
    agc_amp_->update(bands.amplitude);
    float norm_bass = agc_bass_->normalize(bands.bass);
    float norm_mid = agc_mid_->normalize(bands.mid);
    float norm_high = agc_high_->normalize(bands.high);
    float norm_amp = agc_amp_->normalize(bands.amplitude);

    // Beat detection
    bool is_beat = beat_det_->update(norm_bass, now);

    // Publish sensor values
    if (bass_sensor_ != nullptr) bass_sensor_->publish_state(norm_bass);
    if (mid_sensor_ != nullptr) mid_sensor_->publish_state(norm_mid);
    if (high_sensor_ != nullptr) high_sensor_->publish_state(norm_high);
    if (amplitude_sensor_ != nullptr) amplitude_sensor_->publish_state(norm_amp);

    // Publish beat (pulse on, turned off in loop() after duration)
    if (is_beat && beat_sensor_ != nullptr) {
        beat_sensor_->publish_state(true);
        beat_on_ms_ = now;
    }

    // Publish BPM periodically (not every cycle — noisy at high frequency)
    if (bpm_sensor_ != nullptr &&
        (now - last_bpm_publish_ms_) >= BPM_PUBLISH_INTERVAL_MS) {
        float bpm = beat_det_->current_bpm(now);
        bpm_sensor_->publish_state(bpm);
        last_bpm_publish_ms_ = now;
    }
}

void AudioReactiveComponent::reset_agc() {
    ESP_LOGI(TAG, "AGC reset: bass [%.4f, %.4f], mid [%.4f, %.4f], high [%.4f, %.4f], amp [%.4f, %.4f]",
             agc_bass_->current_min(), agc_bass_->current_max(),
             agc_mid_->current_min(), agc_mid_->current_max(),
             agc_high_->current_min(), agc_high_->current_max(),
             agc_amp_->current_min(), agc_amp_->current_max());
    agc_bass_->reset();
    agc_mid_->reset();
    agc_high_->reset();
    agc_amp_->reset();
    beat_det_->reset();
    ESP_LOGI(TAG, "AGC and beat detector reset — re-calibrating");
}

void AudioReactiveBeatSensitivityNumber::setup() {
    // Publish the initial value so HA sees the configured default
    float initial = static_cast<float>(parent_->beat_sensitivity_);
    this->publish_state(initial);
}

void AudioReactiveBeatSensitivityNumber::control(float value) {
    parent_->update_beat_sensitivity(static_cast<int>(value));
    this->publish_state(value);
}

void AudioReactiveComponent::update_beat_sensitivity(int value) {
    beat_sensitivity_ = value;
    if (beat_det_ != nullptr) {
        beat_det_->set_sensitivity(value);
    }
    ESP_LOGI(TAG, "Beat sensitivity changed to %d", value);
}

void AudioReactiveComponent::dump_config() {
    ESP_LOGCONFIG(TAG, "AudioReactive:");
    ESP_LOGCONFIG(TAG, "  FFT size: %u (fixed)", FFT_SIZE);
    ESP_LOGCONFIG(TAG, "  Update interval: %u ms", update_interval_ms_);
    ESP_LOGCONFIG(TAG, "  Beat sensitivity: %d", beat_sensitivity_);
    if (beat_sensitivity_number_ != nullptr) {
        ESP_LOGCONFIG(TAG, "  Sensitivity number entity: attached");
    }
}

}  // namespace audio_reactive
}  // namespace esphome
