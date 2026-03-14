#include "audio_reactive.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace audio_reactive {

static const char *const TAG = "audio_reactive";

void AudioReactiveComponent::setup() {
    ESP_LOGCONFIG(TAG, "Setting up AudioReactive...");

    // Allocate DSP pipeline
    fft_ = new FFTProcessor<FFT_SIZE>(SAMPLE_RATE);
    // BandAggregator uses default constructor (hardcoded 22050 Hz band definitions)
    // AGC instances are stack-allocated with AGC_NORMAL preset
    onset_det_ = new OnsetDetector(beat_sensitivity_);
    silence_det_ = SilenceDetector(squelch_);
    limiter_ = DynamicsLimiter();

    // Register callback for incoming audio data (called from I2S task)
    if (mic_ != nullptr) {
        mic_->add_data_callback([this](const std::vector<uint8_t> &data) {
            if (muted_) return;  // No-op when muted
            const int16_t *samples = reinterpret_cast<const int16_t *>(data.data());
            size_t sample_count = data.size() / sizeof(int16_t);

            // Convert to float and write to ring buffer
            // Process in small batches to avoid large stack allocation
            float temp[64];
            size_t offset = 0;
            while (offset < sample_count) {
                size_t batch = std::min(sample_count - offset, static_cast<size_t>(64));
                for (size_t i = 0; i < batch; i++) {
                    temp[i] = static_cast<float>(samples[offset + i]) / 32768.0f;
                }
                ring_buffer_.write(temp, batch);
                offset += batch;
            }
        });
    } else {
        ESP_LOGW(TAG, "No microphone assigned!");
    }

    // Create FFT processing task pinned to core 0
    xTaskCreatePinnedToCore(fft_task_func, "FFT", 4096, this, 1, &fft_task_handle_, 0);

    ESP_LOGI(TAG, "Initialized (FFT=%u, rate=%.0f, interval=%ums, sensitivity=%d, squelch=%.0f)",
             FFT_SIZE, SAMPLE_RATE, update_interval_ms_, beat_sensitivity_, squelch_);
}

void AudioReactiveComponent::fft_task_func(void *param) {
    auto *self = static_cast<AudioReactiveComponent *>(param);
    float fft_buffer[FFT_SIZE];

    for (;;) {
        // Wait until we have enough samples for a full FFT frame
        if (self->ring_buffer_.available() < FFT_SIZE) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        // Peek 512 samples (don't consume yet — we only advance by HOP_SIZE)
        self->ring_buffer_.peek(fft_buffer, FFT_SIZE);

        // Process FFT
        self->fft_->process(fft_buffer);
        const float *magnitudes = self->fft_->magnitudes();

        // Aggregate into 16 bands
        BandEnergies16 energies = self->band_agg_.aggregate16(magnitudes, self->fft_->bin_count());

        // Store result behind spinlock for main loop
        taskENTER_CRITICAL(&self->fft_mux_);
        self->shared_energies_ = energies;
        self->new_data_available_ = true;
        taskEXIT_CRITICAL(&self->fft_mux_);

        // Advance by HOP_SIZE (75% overlap: 512 - 128 = 384 samples reused)
        self->ring_buffer_.advance(HOP_SIZE);
    }
}

void AudioReactiveComponent::loop() {
    // Start microphone on first loop iteration (after all components are set up)
    if (!mic_started_ && mic_ != nullptr && !muted_) {
        mic_->start();
        mic_started_ = true;
        ESP_LOGI(TAG, "Microphone started");
    }

    uint32_t now = millis();

    // Turn off onset binary sensor after pulse duration
    if (onset_on_ms_ > 0 && (now - onset_on_ms_) >= ONSET_PULSE_DURATION_MS) {
        if (onset_sensor_ != nullptr) {
            onset_sensor_->publish_state(false);
        }
        onset_on_ms_ = 0;
    }

    // If muted, skip processing
    if (muted_) return;

    // Rate limit processing
    if ((now - last_process_ms_) < update_interval_ms_) return;

    // Read latest FFT results from shared struct
    BandEnergies16 energies;
    bool has_data = false;
    taskENTER_CRITICAL(&fft_mux_);
    if (new_data_available_) {
        energies = shared_energies_;
        new_data_available_ = false;
        has_data = true;
    }
    taskEXIT_CRITICAL(&fft_mux_);

    if (!has_data) return;

    last_process_ms_ = now;

    // Silence detection on raw amplitude
    auto silence_result = silence_det_.update(energies.amplitude, now);

    if (silence_result.is_below_gate) {
        // Below squelch gate — suspend AGC, publish zeros
        agc_bass_.suspend();
        agc_mid_.suspend();
        agc_high_.suspend();
        agc_amp_.suspend();

        publish_zeros_();

        // Update silence sensor (edge-triggered)
        if (silence_sensor_ != nullptr && silence_result.is_silent != prev_silence_) {
            silence_sensor_->publish_state(silence_result.is_silent);
        }
        prev_silence_ = silence_result.is_silent;
        return;
    }

    // Not silent — clear silence state
    if (silence_sensor_ != nullptr && prev_silence_) {
        silence_sensor_->publish_state(false);
    }
    prev_silence_ = false;

    // AGC normalization — each band uses its own AGC instance
    float norm_bass = agc_bass_.process(energies.bass);
    float norm_mid = agc_mid_.process(energies.mid);
    float norm_high = agc_high_.process(energies.high);
    float norm_amp = agc_amp_.process(energies.amplitude);

    // Dynamics limiter on amplitude
    norm_amp = limiter_.process(norm_amp, static_cast<float>(update_interval_ms_));

    // Asymmetric EMA smoothing: fast rise, slow fall
    smooth_bass_ = asymmetric_ema(norm_bass, smooth_bass_);
    smooth_mid_ = asymmetric_ema(norm_mid, smooth_mid_);
    smooth_high_ = asymmetric_ema(norm_high, smooth_high_);
    smooth_amp_ = asymmetric_ema(norm_amp, smooth_amp_);

    // Onset detection using AGC-normalized 16-band energies
    float agc_bands[16];
    for (int i = 0; i < 16; i++) {
        // Simple per-band normalization using the appropriate summary AGC gain
        // Bands 0-3 use bass gain, 4-9 use mid gain, 10-15 use high gain
        float gain;
        if (i < 4) gain = agc_bass_.current_gain();
        else if (i < 10) gain = agc_mid_.current_gain();
        else gain = agc_high_.current_gain();
        agc_bands[i] = std::min(1.0f, energies.bands[i] * gain);
    }
    auto onset_result = onset_det_->update(agc_bands, smooth_bass_, now);

    // Publish sensor values
    if (bass_sensor_ != nullptr) bass_sensor_->publish_state(smooth_bass_);
    if (mid_sensor_ != nullptr) mid_sensor_->publish_state(smooth_mid_);
    if (high_sensor_ != nullptr) high_sensor_->publish_state(smooth_high_);
    if (amplitude_sensor_ != nullptr) amplitude_sensor_->publish_state(smooth_amp_);

    // Publish onset (pulse on, turned off in loop() after duration)
    if (onset_result.detected && onset_sensor_ != nullptr) {
        onset_sensor_->publish_state(true);
        onset_on_ms_ = now;
    }

    // Publish BPM periodically
    if (bpm_sensor_ != nullptr &&
        (now - last_bpm_publish_ms_) >= BPM_PUBLISH_INTERVAL_MS) {
        float bpm = onset_det_->current_bpm(now);
        bpm_sensor_->publish_state(bpm);
        last_bpm_publish_ms_ = now;
    }
}

void AudioReactiveComponent::publish_zeros_() {
    if (bass_sensor_ != nullptr) bass_sensor_->publish_state(0.0f);
    if (mid_sensor_ != nullptr) mid_sensor_->publish_state(0.0f);
    if (high_sensor_ != nullptr) high_sensor_->publish_state(0.0f);
    if (amplitude_sensor_ != nullptr) amplitude_sensor_->publish_state(0.0f);
}

void AudioReactiveComponent::set_muted(bool muted) {
    if (muted_ == muted) return;
    muted_ = muted;

    if (muted) {
        // Stop mic and clear state
        if (mic_ != nullptr && mic_started_) {
            mic_->stop();
            mic_started_ = false;
        }
        ring_buffer_.clear();

        // Publish zeros for all sensors
        publish_zeros_();

        // Set silence on
        if (silence_sensor_ != nullptr) {
            silence_sensor_->publish_state(true);
        }
        prev_silence_ = true;

        // Zero smoothed values
        smooth_bass_ = 0.0f;
        smooth_mid_ = 0.0f;
        smooth_high_ = 0.0f;
        smooth_amp_ = 0.0f;

        ESP_LOGI(TAG, "Microphone muted");
    } else {
        // Restart mic and reset calibration
        if (mic_ != nullptr) {
            mic_->start();
            mic_started_ = true;
        }
        reset_agc();

        if (silence_sensor_ != nullptr) {
            silence_sensor_->publish_state(false);
        }
        prev_silence_ = false;

        ESP_LOGI(TAG, "Microphone unmuted");
    }

    // Publish mute switch state
    if (mute_switch_ != nullptr) {
        mute_switch_->publish_state(muted);
    }
}

void AudioReactiveComponent::toggle_mute() {
    set_muted(!muted_);
}

void AudioReactiveComponent::reset_agc() {
    ESP_LOGI(TAG, "Resetting AGC and onset detector");
    agc_bass_.reset();
    agc_mid_.reset();
    agc_high_.reset();
    agc_amp_.reset();
    if (onset_det_ != nullptr) {
        onset_det_->reset();
    }
    limiter_.reset();
    smooth_bass_ = 0.0f;
    smooth_mid_ = 0.0f;
    smooth_high_ = 0.0f;
    smooth_amp_ = 0.0f;
    ESP_LOGI(TAG, "AGC and onset detector reset - re-calibrating");
}

void AudioReactiveComponent::update_beat_sensitivity(int value) {
    beat_sensitivity_ = value;
    if (onset_det_ != nullptr) {
        onset_det_->set_sensitivity(value);
    }
    ESP_LOGI(TAG, "Beat sensitivity changed to %d", value);
}

// --- Platform entity implementations ---

void AudioReactiveBeatSensitivityNumber::setup() {
    float initial = static_cast<float>(parent_->beat_sensitivity_);
    this->publish_state(initial);
}

void AudioReactiveBeatSensitivityNumber::control(float value) {
    parent_->update_beat_sensitivity(static_cast<int>(value));
    this->publish_state(value);
}

void AudioReactiveMicrophoneMuteSwitch::setup() {
    this->publish_state(false);
}

void AudioReactiveMicrophoneMuteSwitch::write_state(bool state) {
    parent_->set_muted(state);
    this->publish_state(state);
}

void AudioReactiveResetAGCButton::press_action() {
    parent_->reset_agc();
}

void AudioReactiveSquelchNumber::setup() {
    this->publish_state(parent_->squelch_);
}

void AudioReactiveSquelchNumber::control(float value) {
    parent_->silence_det_.set_squelch(value);
    parent_->squelch_ = value;
    this->publish_state(value);
}

void AudioReactiveDetectionModeSelect::setup() {
    this->publish_state("spectral_flux");
}

void AudioReactiveDetectionModeSelect::control(const std::string &value) {
    if (value == "spectral_flux") {
        parent_->onset_det_->set_mode(OnsetDetector::MODE_SPECTRAL_FLUX);
    } else if (value == "bass_energy") {
        parent_->onset_det_->set_mode(OnsetDetector::MODE_BASS_ENERGY);
    }
    this->publish_state(value);
}

void AudioReactiveComponent::dump_config() {
    ESP_LOGCONFIG(TAG, "AudioReactive:");
    ESP_LOGCONFIG(TAG, "  FFT size: %u (fixed)", FFT_SIZE);
    ESP_LOGCONFIG(TAG, "  Sample rate: %.0f Hz", SAMPLE_RATE);
    ESP_LOGCONFIG(TAG, "  Update interval: %u ms", update_interval_ms_);
    ESP_LOGCONFIG(TAG, "  Beat sensitivity: %d", beat_sensitivity_);
    ESP_LOGCONFIG(TAG, "  Squelch: %.0f", squelch_);
    if (beat_sensitivity_number_ != nullptr) {
        ESP_LOGCONFIG(TAG, "  Sensitivity number entity: attached");
    }
    if (squelch_number_ != nullptr) {
        ESP_LOGCONFIG(TAG, "  Squelch number entity: attached");
    }
    if (mute_switch_ != nullptr) {
        ESP_LOGCONFIG(TAG, "  Mute switch entity: attached");
    }
    if (reset_button_ != nullptr) {
        ESP_LOGCONFIG(TAG, "  Reset AGC button entity: attached");
    }
    if (detection_mode_select_ != nullptr) {
        ESP_LOGCONFIG(TAG, "  Detection mode select entity: attached");
    }
}

}  // namespace audio_reactive
}  // namespace esphome
