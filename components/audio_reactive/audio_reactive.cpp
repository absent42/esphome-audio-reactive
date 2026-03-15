#include "audio_reactive.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace audio_reactive {

static const char *const TAG = "audio_reactive";

void AudioReactiveComponent::setup() {
    ESP_LOGCONFIG(TAG, "Setting up AudioReactive...");

    // Allocate DSP pipeline
    fft_ = new FFTProcessor<512>(sample_rate_);  // Template param fixed at 512 for now
    band_agg_ = BandAggregator(sample_rate_, fft_size_);
    // AGC instances are stack-allocated with AGC_NORMAL preset
    // Set per-band noise floors calibrated to PDM mic quiet room levels.
    // Values are PRE-SCALED (raw / 20): quiet bass=0.15-2.25, mid=0.04-0.15, high=0.01-0.04
    // These floors prevent AGC from amplifying mic self-noise to 1.0
    agc_bass_.set_noise_floor(0.25f);  // Bass has high noise from AC hum
    agc_mid_.set_noise_floor(0.08f);   // Mid is cleaner
    agc_high_.set_noise_floor(0.03f);  // High is cleanest
    agc_amp_.set_noise_floor(0.10f);   // Overall amplitude
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

    // Load calibration from flash (NVS)
    cal_pref_ = global_preferences->make_preference<CalibrationStore>(fnv1_hash("audio_cal"));
    if (cal_pref_.load(&cal_store_)) {
        ESP_LOGI(TAG, "Loaded calibration from flash: quiet=%s music=%s scale=%.4f squelch_thresh=%.2f",
                 cal_store_.quiet_calibrated ? "yes" : "no",
                 cal_store_.music_calibrated ? "yes" : "no",
                 cal_store_.raw_scale, cal_store_.squelch_threshold);
        apply_calibration();
    } else {
        ESP_LOGI(TAG, "No stored calibration — using defaults");
        cal_store_ = {5.0f, 0.25f, 0.08f, 0.03f, 0.10f, 1.0f / 20.0f, false, false};
    }

    ESP_LOGI(TAG, "Initialized (FFT=%u, rate=%.0f, hop=%u, interval=%ums, sensitivity=%d, squelch=%.0f)",
             fft_size_, sample_rate_, hop_size_, update_interval_ms_, beat_sensitivity_, squelch_);
}

void AudioReactiveComponent::fft_task_func(void *param) {
    auto *self = static_cast<AudioReactiveComponent *>(param);
    // Stack buffer sized for FFT template (fixed at 512 for now)
    float fft_buffer[512];

    for (;;) {
        // Wait until we have enough samples for a full FFT frame
        if (self->ring_buffer_.available() < self->fft_size_) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        // Peek fft_size_ samples (don't consume yet — we only advance by hop_size_)
        self->ring_buffer_.peek(fft_buffer, self->fft_size_);

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

        // Advance by hop_size_ (75% overlap)
        self->ring_buffer_.advance(self->hop_size_);
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

    // Debug: comprehensive logging every 2 seconds (gated by debug_logging_)
    if (debug_logging_) {
        static uint32_t last_debug_ms = 0;
        static float raw_amp_min = 1e10f, raw_amp_max = 0.0f;
        static float raw_bass_min = 1e10f, raw_bass_max = 0.0f;
        // Track min/max of raw values between log intervals
        if (energies.amplitude < raw_amp_min) raw_amp_min = energies.amplitude;
        if (energies.amplitude > raw_amp_max) raw_amp_max = energies.amplitude;
        if (energies.bass < raw_bass_min) raw_bass_min = energies.bass;
        if (energies.bass > raw_bass_max) raw_bass_max = energies.bass;

        if ((now - last_debug_ms) >= 2000) {
            ESP_LOGI(TAG, "=== AUDIO DEBUG === (sample_rate=%.0f, fft_size=%u)", sample_rate_, fft_size_);
            ESP_LOGI(TAG, "RAW (current): amp=%.4f bass=%.4f mid=%.4f high=%.4f",
                     energies.amplitude, energies.bass, energies.mid, energies.high);
            ESP_LOGI(TAG, "RAW (2s range): amp=[%.4f..%.4f] bass=[%.4f..%.4f]",
                     raw_amp_min, raw_amp_max, raw_bass_min, raw_bass_max);
            ESP_LOGI(TAG, "RAW bands: [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
                     energies.bands[0], energies.bands[1], energies.bands[2], energies.bands[3],
                     energies.bands[4], energies.bands[5], energies.bands[6], energies.bands[7],
                     energies.bands[8], energies.bands[9], energies.bands[10], energies.bands[11],
                     energies.bands[12], energies.bands[13], energies.bands[14], energies.bands[15]);
            ESP_LOGI(TAG, "SCALED: bass=%.3f mid=%.3f high=%.3f amp=%.3f (raw_scale=%.4f)",
                     energies.bass * raw_scale_, energies.mid * raw_scale_,
                     energies.high * raw_scale_, energies.amplitude * raw_scale_, raw_scale_);
            ESP_LOGI(TAG, "AGC gains: bass=%.2f mid=%.2f high=%.2f amp=%.2f",
                     agc_bass_.current_gain(), agc_mid_.current_gain(),
                     agc_high_.current_gain(), agc_amp_.current_gain());
            float dbg_silence_signal = energies.mid + energies.high;
            ESP_LOGI(TAG, "Silence signal (mid+high)=%.2f | Squelch=%.1f (threshold=%.2f)",
                     dbg_silence_signal, silence_det_.squelch(), silence_det_.squelch() * 0.5f);
            ESP_LOGI(TAG, "Silence state: prev_silence=%d", prev_silence_);
            ESP_LOGI(TAG, "Calibration: quiet=%s music=%s scale=%.4f squelch_thresh=%.2f",
                     cal_store_.quiet_calibrated ? "yes" : "no",
                     cal_store_.music_calibrated ? "yes" : "no",
                     cal_store_.raw_scale, cal_store_.squelch_threshold);
            ESP_LOGI(TAG, "Published: bass=%.3f mid=%.3f high=%.3f amp=%.3f",
                     smooth_bass_, smooth_mid_, smooth_high_, smooth_amp_);
            ESP_LOGI(TAG, "Ring buffer: %u/%u samples",
                     static_cast<unsigned>(ring_buffer_.available()),
                     static_cast<unsigned>(ring_buffer_.capacity()));
            // Reset min/max trackers
            raw_amp_min = 1e10f; raw_amp_max = 0.0f;
            raw_bass_min = 1e10f; raw_bass_max = 0.0f;
            last_debug_ms = now;
        }
    }

    // Calibration accumulation — runs BEFORE silence gate so quiet room
    // calibration can sample even during "silence"
    if (cal_state_ != CAL_IDLE) {
        uint32_t elapsed = now - cal_start_ms_;

        if (cal_state_ == CAL_QUIET) {
            float mid_high = energies.mid + energies.high;
            cal_max_mid_high_ = std::max(cal_max_mid_high_, mid_high);
            cal_max_bass_ = std::max(cal_max_bass_, energies.bass);
            cal_max_mid_ = std::max(cal_max_mid_, energies.mid);
            cal_max_high_ = std::max(cal_max_high_, energies.high);
            cal_max_amp_ = std::max(cal_max_amp_, energies.amplitude);

            if (elapsed >= 3000) {
                finish_quiet_calibration();
            }
        } else if (cal_state_ == CAL_MUSIC) {
            cal_sum_amp_ += energies.amplitude;
            cal_max_bass_ = std::max(cal_max_bass_, energies.bass);
            cal_max_mid_ = std::max(cal_max_mid_, energies.mid);
            cal_max_high_ = std::max(cal_max_high_, energies.high);
            cal_sample_count_++;

            if (elapsed >= 5000) {
                finish_music_calibration();
            }
        }
    }

    // Silence detection on mid+high energy (not amplitude, which is diluted
    // by empty high-frequency bins, and not bass, which picks up AC hum/rumble).
    // Quiet room mid+high ≈ 1-4, music ≈ 10-25.
    float silence_signal = energies.mid + energies.high;
    auto silence_result = silence_det_.update(silence_signal, now);

    if (silence_result.is_below_gate) {
        // Below squelch gate — suspend AGC, publish zeros
        agc_bass_.suspend();
        agc_mid_.suspend();
        agc_high_.suspend();
        agc_amp_.suspend();

        publish_zeros_();

        // Reset BPM on silence transition
        if (silence_result.is_silent && !prev_silence_) {
            if (bpm_sensor_ != nullptr) bpm_sensor_->publish_state(0.0f);
            if (onset_det_ != nullptr) onset_det_->reset();
        }

        // Update silence sensor (edge-triggered)
        if (silence_result.is_silent != prev_silence_) {
            if (silence_sensor_ != nullptr) {
                silence_sensor_->publish_state(silence_result.is_silent);
            }
            on_silence_changed_callbacks_.call();
        }
        prev_silence_ = silence_result.is_silent;
        return;
    }

    // Not silent — clear silence state
    if (prev_silence_) {
        if (silence_sensor_ != nullptr) {
            silence_sensor_->publish_state(false);
        }
        on_silence_changed_callbacks_.call();
    }
    prev_silence_ = false;

    // Pre-scale raw FFT magnitudes to ~0-1 range before AGC.
    // raw_scale_ defaults to 1/20 but is updated by music calibration
    // to map typical playback levels to ~0.5 (AGC target).
    float scaled_bass = energies.bass * raw_scale_;
    float scaled_mid = energies.mid * raw_scale_;
    float scaled_high = energies.high * raw_scale_;
    float scaled_amp = energies.amplitude * raw_scale_;

    // AGC normalization — each band uses its own AGC instance
    float norm_bass = agc_bass_.process(scaled_bass);
    float norm_mid = agc_mid_.process(scaled_mid);
    float norm_high = agc_high_.process(scaled_high);
    float norm_amp = agc_amp_.process(scaled_amp);

    // Dynamics limiter on amplitude
    norm_amp = limiter_.process(norm_amp, static_cast<float>(update_interval_ms_));

    // Asymmetric EMA smoothing: fast rise, slow fall
    smooth_bass_ = asymmetric_ema(norm_bass, smooth_bass_);
    smooth_mid_ = asymmetric_ema(norm_mid, smooth_mid_);
    smooth_high_ = asymmetric_ema(norm_high, smooth_high_);
    smooth_amp_ = asymmetric_ema(norm_amp, smooth_amp_);

    // Onset detection using scaled 16-band energies
    float scaled_bands[16];
    for (int i = 0; i < 16; i++) {
        scaled_bands[i] = energies.bands[i] * raw_scale_;
    }
    auto onset_result = onset_det_->update(scaled_bands, smooth_bass_, now);

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

        // Publish zeros for all sensors including BPM
        publish_zeros_();
        if (bpm_sensor_ != nullptr) bpm_sensor_->publish_state(0.0f);
        if (onset_det_ != nullptr) onset_det_->reset();

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

    on_mute_changed_callbacks_.call();
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

// --- Calibration methods ---

void AudioReactiveComponent::start_quiet_calibration() {
    cal_state_ = CAL_QUIET;
    cal_start_ms_ = millis();
    cal_max_mid_high_ = 0;
    cal_max_bass_ = 0;
    cal_max_mid_ = 0;
    cal_max_high_ = 0;
    cal_max_amp_ = 0;
    ESP_LOGI(TAG, "Quiet room calibration started (3 seconds)...");
    on_calibration_started_callbacks_.call();
}

void AudioReactiveComponent::finish_quiet_calibration() {
    cal_state_ = CAL_IDLE;

    // Compute thresholds
    cal_store_.squelch_threshold = cal_max_mid_high_ * 1.5f;
    float scale = cal_store_.music_calibrated ? cal_store_.raw_scale : (1.0f / 20.0f);
    cal_store_.noise_floor_bass = (cal_max_bass_ * scale) * 1.2f;   // 20% headroom
    cal_store_.noise_floor_mid = (cal_max_mid_ * scale) * 1.2f;
    cal_store_.noise_floor_high = (cal_max_high_ * scale) * 1.2f;
    cal_store_.noise_floor_amp = (cal_max_amp_ * scale) * 1.2f;
    cal_store_.quiet_calibrated = true;

    // Apply and save
    apply_calibration();
    cal_pref_.save(&cal_store_);

    ESP_LOGI(TAG, "Quiet calibration done: squelch_threshold=%.2f, noise_floors: bass=%.3f mid=%.3f high=%.3f amp=%.3f",
             cal_store_.squelch_threshold, cal_store_.noise_floor_bass,
             cal_store_.noise_floor_mid, cal_store_.noise_floor_high, cal_store_.noise_floor_amp);
    on_calibration_complete_callbacks_.call();
}

void AudioReactiveComponent::start_music_calibration() {
    cal_state_ = CAL_MUSIC;
    cal_start_ms_ = millis();
    cal_sum_amp_ = 0;
    cal_max_bass_ = 0;
    cal_max_mid_ = 0;
    cal_max_high_ = 0;
    cal_sample_count_ = 0;
    ESP_LOGI(TAG, "Music calibration started (5 seconds) — play music at typical volume...");
    on_calibration_started_callbacks_.call();
}

void AudioReactiveComponent::finish_music_calibration() {
    cal_state_ = CAL_IDLE;

    if (cal_sample_count_ > 0) {
        float avg_amp = cal_sum_amp_ / cal_sample_count_;
        // Scale so typical music amplitude maps to ~0.5 (AGC target)
        cal_store_.raw_scale = (avg_amp > 0.01f) ? (0.5f / avg_amp) : (1.0f / 20.0f);
        cal_store_.music_calibrated = true;

        apply_calibration();
        cal_pref_.save(&cal_store_);

        ESP_LOGI(TAG, "Music calibration done: avg_amp=%.2f, raw_scale=%.4f",
                 avg_amp, cal_store_.raw_scale);
        on_calibration_complete_callbacks_.call();
    }
}

void AudioReactiveComponent::apply_calibration() {
    // Apply stored calibration to the DSP pipeline
    float scale = cal_store_.raw_scale;
    agc_bass_.set_noise_floor(cal_store_.noise_floor_bass);
    agc_mid_.set_noise_floor(cal_store_.noise_floor_mid);
    agc_high_.set_noise_floor(cal_store_.noise_floor_high);
    agc_amp_.set_noise_floor(cal_store_.noise_floor_amp);
    // raw_scale_ is used in the main loop for pre-scaling
    raw_scale_ = scale;
    // Update silence detector squelch from calibration
    if (cal_store_.quiet_calibrated) {
        silence_det_.set_squelch_threshold_direct(cal_store_.squelch_threshold);
    }

    // Reset AGC to start fresh with new settings
    reset_agc();
}

void AudioReactiveCalibrateQuietButton::press_action() {
    parent_->start_quiet_calibration();
}

void AudioReactiveCalibrateMusictButton::press_action() {
    parent_->start_music_calibration();
}

void AudioReactiveComponent::dump_config() {
    ESP_LOGCONFIG(TAG, "AudioReactive:");
    ESP_LOGCONFIG(TAG, "  Sample rate: %.0f Hz", sample_rate_);
    ESP_LOGCONFIG(TAG, "  FFT size: %u", fft_size_);
    ESP_LOGCONFIG(TAG, "  Hop size: %u", hop_size_);
    ESP_LOGCONFIG(TAG, "  Debug logging: %s", debug_logging_ ? "yes" : "no");
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
    if (calibrate_quiet_button_ != nullptr) {
        ESP_LOGCONFIG(TAG, "  Calibrate quiet button entity: attached");
    }
    if (calibrate_music_button_ != nullptr) {
        ESP_LOGCONFIG(TAG, "  Calibrate music button entity: attached");
    }
    ESP_LOGCONFIG(TAG, "  Calibration: quiet=%s music=%s raw_scale=%.4f",
                  cal_store_.quiet_calibrated ? "yes" : "no",
                  cal_store_.music_calibrated ? "yes" : "no",
                  cal_store_.raw_scale);
}

}  // namespace audio_reactive
}  // namespace esphome
