#include "audio_reactive.h"
#include "spectral_descriptors.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"
#include "esphome/core/helpers.h"
#include "esp_timer.h"
#include <cmath>
#include <cstring>

namespace esphome {
namespace audio_reactive {

static const char *const TAG = "audio_reactive";

void AudioReactiveComponent::setup() {
    ESP_LOGCONFIG(TAG, "Setting up AudioReactive...");

    // Allocate DSP pipeline
    fft_ = new FFTProcessor<FFT_SIZE>(sample_rate_);
    fft_buffer_ = new float[FFT_SIZE];
    whitened_mags_ = new float[FFT_SIZE / 2];
    band_agg_ = BandAggregator(sample_rate_, FFT_SIZE);
    float hop_rate = sample_rate_ / static_cast<float>(HOP_SIZE);
    whitening_ = SpectralWhitening<256>(hop_rate, 2.0f);
    // AGC instances are stack-allocated with AGC_NORMAL preset
    // Set per-band noise floors calibrated to PDM mic quiet room levels.
    // Values are PRE-SCALED (raw / 20): quiet bass=0.15-2.25, mid=0.04-0.15, high=0.01-0.04
    // These floors prevent AGC from amplifying mic self-noise to 1.0
    agc_bass_.set_noise_floor(0.25f);  // Bass has high noise from AC hum
    agc_mid_.set_noise_floor(0.08f);   // Mid is cleaner
    agc_high_.set_noise_floor(0.03f);  // High is cleanest
    agc_amp_.set_noise_floor(0.10f);   // Overall amplitude
    onset_det_ = new OnsetDetector(beat_sensitivity_);
    float update_hz = 1000.0f / static_cast<float>(update_interval_ms_);
    beat_tracker_ = new BeatTracker(update_hz);
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
                size_t written = ring_buffer_.write(temp, batch);
                if (written < batch) {
                    // Split load/add/store: compound assignment on a
                    // volatile is deprecated since C++20 (ESP-IDF 5.x
                    // builds with gnu++20).
                    const uint32_t dropped = ring_dropped_samples_;
                    ring_dropped_samples_ =
                        dropped + static_cast<uint32_t>(batch - written);
                }
                offset += batch;
            }
            // Wake FFT task when enough samples are available
            if (ring_buffer_.available() >= FFT_SIZE && fft_task_handle_ != nullptr) {
                xTaskNotifyGive(fft_task_handle_);
            }
        });
    } else {
        ESP_LOGW(TAG, "No microphone assigned!");
    }

#ifdef AUDIO_REACTIVE_PRO
    // Pro-tier DSP initialization — must happen before the FFT task starts so it
    // has valid mel_fb_ coefficients and a mags_sq_ buffer to write into.
    // Uses default heap (in PSRAM on S3R via ESP-IDF allocator with SPIRAM_USE_MALLOC enabled).
    mags_sq_ = new float[fft_->bin_count()]();
    // freq_min=80 Hz aligns mel slot 0 with the realistic low-end response of
    // the supported mics: PDM MEMS (SPM1423) is -3 dB at ~100 Hz and steeply
    // attenuated below; codec-driven mics (ES7210/ES7243) typically have a
    // 50-100 Hz HPF. Below 80 Hz is mostly AC line hum (50/60 Hz) and mic
    // self-noise rather than musical content. With freq_min=80, mel slot 0
    // (the "low_bass" musical band) spans ~80-240 Hz with peak sensitivity
    // near 156 Hz - a reasonable kick-drum / bass-guitar fundamental range.
    mel_fb_.setup(sample_rate_, 80.0f, 16000.0f);
    musical_bands_.reset();
    musical_bands_.set_noise_floors(cal_store_v2_.noise_floor);
    ESP_LOGI(TAG, "Pro-tier DSP initialized: FFT=%u, N_MEL=%u, bands=%u",
             FFT_SIZE, N_MEL, MusicalBands::kNumBands);
#endif

    // Create FFT processing task pinned to core 0
    xTaskCreatePinnedToCore(fft_task_func, "FFT", 6144, this, 1, &fft_task_handle_, 0);

    // Load calibration from flash (NVS)
#ifdef AUDIO_REACTIVE_PRO
    // Pro tier: try V2 first, then V1 migration, then uncalibrated defaults.
    cal_pref_v2_ = global_preferences->make_preference<CalibrationStoreV2>(fnv1_hash("audio_cal_v2"));
    cal_pref_ = global_preferences->make_preference<CalibrationStore>(fnv1_hash("audio_cal"));
    bool v2_loaded = cal_pref_v2_.load(&cal_store_v2_) && cal_store_v2_.version == 2;
    // If a future V3 supersedes V2, insert the version bump migration here
    // before declaring V2 usable.

    if (v2_loaded) {
        ESP_LOGI(TAG, "Loaded V2 calibration: quiet=%s music=%s scale=%.4f squelch_thresh=%.2f",
                 cal_store_v2_.quiet_calibrated ? "yes" : "no",
                 cal_store_v2_.music_calibrated ? "yes" : "no",
                 cal_store_v2_.raw_scale, cal_store_v2_.squelch_threshold);
        // Mirror V2 -> V1 so the shared apply path (apply_calibration) gives
        // agc_bass_/mid_/high_/amp_ non-zero floors derived from the musical-band floors.
        cal_store_.squelch_threshold = cal_store_v2_.squelch_threshold;
        cal_store_.noise_floor_bass = cal_store_v2_.noise_floor[1];   // bass
        cal_store_.noise_floor_mid  = cal_store_v2_.noise_floor[3];   // mid
        cal_store_.noise_floor_high = cal_store_v2_.noise_floor[5];   // high
        cal_store_.noise_floor_amp  = (cal_store_v2_.noise_floor[1] + cal_store_v2_.noise_floor[3] + cal_store_v2_.noise_floor[5]) / 3.0f;
        cal_store_.raw_scale = cal_store_v2_.raw_scale;
        cal_store_.quiet_calibrated = cal_store_v2_.quiet_calibrated;
        cal_store_.music_calibrated = cal_store_v2_.music_calibrated;
        cal_stale_ = !(cal_store_v2_.quiet_calibrated && cal_store_v2_.music_calibrated);
        apply_calibration_v2_();
    } else if (cal_pref_.load(&cal_store_) && migrate_v1_to_v2(cal_store_, cal_store_v2_)) {
        ESP_LOGW(TAG, "Migrated V1 calibration to V2 (linear-interpolated noise floors). "
                     "calibration_stale=true - user should re-run calibration.");
        cal_stale_ = true;
        cal_pref_v2_.save(&cal_store_v2_);
        apply_calibration_v2_();
    } else {
        ESP_LOGI(TAG, "No stored V2/V1 calibration - using uncalibrated defaults (calibration_stale=true)");
        cal_store_v2_ = {2, {0, 0, 0}, 5.0f, {0, 0, 0, 0, 0, 0, 0}, 1.0f / 20.0f, false, false, {0, 0}};
        cal_store_ = {5.0f, 0.25f, 0.08f, 0.03f, 0.10f, 1.0f / 20.0f, false, false};
        cal_stale_ = true;
    }
    if (calibration_stale_sensor_ != nullptr) {
        calibration_stale_sensor_->publish_state(cal_stale_);
    }
#else
    // Basic tier: unchanged V1 load path.
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
#endif

    ESP_LOGI(TAG, "Initialized (FFT=%u, rate=%.0f, hop=%u, interval=%ums, sensitivity=%d, squelch=%.0f)",
             FFT_SIZE, sample_rate_, HOP_SIZE, update_interval_ms_, beat_sensitivity_, squelch_);
}

void AudioReactiveComponent::fft_task_func(void *param) {
    auto *self = static_cast<AudioReactiveComponent *>(param);
    float *fft_buffer = self->fft_buffer_;

    for (;;) {
        // Wait until notified that enough samples are available
        if (self->ring_buffer_.available() < FFT_SIZE) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));  // 100ms timeout as safety fallback
            continue;
        }

        // Capture start AFTER the wait path returns — instrumentation must measure only
        // FFT + DSP processing time, never blocking sleep. The peak/mean stats are only
        // meaningful if this timer does not include ulTaskNotifyTake() wait latency.
        // Gated on sensors being wired so users who don't opt-in pay zero overhead.
        bool measure_cycle = (self->fft_task_cycle_mean_sensor_ != nullptr) ||
                             (self->fft_task_cycle_peak_sensor_ != nullptr);
        uint64_t start_us = measure_cycle ? esp_timer_get_time() : 0;

        // Peek FFT_SIZE samples (don't consume yet — we only advance by HOP_SIZE)
        self->ring_buffer_.peek(fft_buffer, FFT_SIZE);

        // Process FFT
        self->fft_->process(fft_buffer);
        const float *magnitudes = self->fft_->magnitudes();

        // Guard against NaN from FFT (can occur during mic startup transients)
        bool has_nan = false;
        for (size_t i = 0; i < self->fft_->bin_count() && !has_nan; i++) {
            if (std::isnan(magnitudes[i])) has_nan = true;
        }
        if (has_nan) {
            self->ring_buffer_.advance(HOP_SIZE);
            continue;
        }

        // Aggregate into 16 bands
        BandEnergies16 energies = self->band_agg_.aggregate16(magnitudes, self->fft_->bin_count());

        // Compute spectral descriptors
        energies.centroid = spectral_centroid(magnitudes, self->fft_->bin_count(), self->band_agg_.hz_per_bin());
        energies.rolloff = spectral_rolloff(magnitudes, self->fft_->bin_count(), self->band_agg_.hz_per_bin());

        size_t bins = self->fft_->bin_count();
#ifndef AUDIO_REACTIVE_PRO
        float complex_onset = 0.0f;

        // Basic-tier complex-domain onset detection (pro tier uses SuperFlux instead, below)
        // Whitened magnitudes for onset detection (original magnitudes used for band energies)
        float *whitened_mags = self->whitened_mags_;
        std::memcpy(whitened_mags, magnitudes, bins * sizeof(float));
        self->whitening_.process(whitened_mags, bins);

        // Complex domain onset using whitened magnitudes
        const float *phases = self->fft_->phases();

        if (self->has_prev_frame_) {
            // Phase advance per bin per hop for a stationary sinusoid (Dixon 2006).
            // Without this, every stable tone looks like an onset due to phase rotation.
            float phase_inc = 2.0f * static_cast<float>(M_PI) *
                              static_cast<float>(self->HOP_SIZE) /
                              static_cast<float>(self->FFT_SIZE);
            for (size_t i = 1; i < bins; i++) {
                float expected_phase = self->prev_phases_[i] + static_cast<float>(i) * phase_inc;
                float predicted_re = self->prev_magnitudes_[i] * cosf(expected_phase);
                float predicted_im = self->prev_magnitudes_[i] * sinf(expected_phase);
                float actual_re = whitened_mags[i] * cosf(phases[i]);
                float actual_im = whitened_mags[i] * sinf(phases[i]);
                float d_re = actual_re - predicted_re;
                float d_im = actual_im - predicted_im;
                complex_onset += sqrtf(d_re * d_re + d_im * d_im);
            }
        }

        // Store whitened frame for next iteration's prediction
        std::memcpy(self->prev_phases_, phases, bins * sizeof(float));
        std::memcpy(self->prev_magnitudes_, whitened_mags, bins * sizeof(float));
        self->has_prev_frame_ = true;
#endif

#ifdef AUDIO_REACTIVE_PRO
        // Pro-tier: compute squared magnitudes into the pre-allocated class buffer
        // (not stack — fft_task_func has only 6144 bytes of stack).
        for (size_t i = 0; i < bins; i++) {
            self->mags_sq_[i] = magnitudes[i] * magnitudes[i];
        }
#endif

        // Store result via double-buffer (no spinlock needed)
        int write_slot = self->shared_write_idx_ ^ 1;
        self->shared_frames_[write_slot].energies = energies;
#ifndef AUDIO_REACTIVE_PRO
        self->shared_frames_[write_slot].complex_onset = complex_onset;
#endif
#ifdef AUDIO_REACTIVE_PRO
        // Populate mel frame into the same write_slot — piggybacks on the existing
        // atomic-index-swap double-buffer for synchronization.
        self->mel_fb_.process(self->mags_sq_, self->shared_frames_[write_slot].mel_frame);
        // SuperFlux must run after mel_frame is written and before the atomic publish
        // below so readers see a consistent snapshot (mel + flux + event together).
        auto sf = self->superflux_.process(self->shared_frames_[write_slot].mel_frame);
        self->shared_frames_[write_slot].superflux_strength = sf.strength;
        self->shared_frames_[write_slot].superflux_event = sf.event;
        // BTrack consumes the SuperFlux strength envelope and emits bpm/phase/conf/event.
        // Must run before the atomic publish so readers see a consistent snapshot.
        auto bt = self->btrack_.process(sf.strength);
        self->shared_frames_[write_slot].btrack_bpm = bt.bpm;
        self->shared_frames_[write_slot].btrack_phase = bt.beat_phase;
        self->shared_frames_[write_slot].btrack_confidence = bt.confidence;
        self->shared_frames_[write_slot].btrack_event = bt.beat_event;
        // frame_id is monotonic, written last so the main loop can use it as a
        // "new data" indicator with a simple inequality check.
        self->shared_frames_[write_slot].frame_id = ++self->next_frame_id_;
#endif
        self->shared_write_idx_ = write_slot;
        self->new_data_available_ = true;

        // Drain cycle timing into shared counters. Gated on measure_cycle so the
        // esp_timer_get_time() call is skipped entirely when no sensor is wired.
        if (measure_cycle) {
            uint64_t end_us = esp_timer_get_time();
            uint32_t delta_us = static_cast<uint32_t>(end_us - start_us);
            self->fft_cycle_total_us_ += delta_us;
            self->fft_cycle_samples_++;
            if (delta_us > self->fft_cycle_peak_us_) self->fft_cycle_peak_us_ = delta_us;
        }

        // Advance by HOP_SIZE (75% overlap)
        self->ring_buffer_.advance(self->HOP_SIZE);
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

    // FFT cycle stats: drain 1s window and publish mean/peak. Counters are
    // written only from fft_task_func; reading them here on the main core is
    // racy in principle but the values are uint32_t and tearing would at worst
    // produce a momentarily stale sample — acceptable for a 1Hz diagnostic.
    if (now - fft_cycle_last_publish_ms_ >= 1000) {
        if (fft_cycle_samples_ > 0) {
            float mean_us = static_cast<float>(fft_cycle_total_us_) / fft_cycle_samples_;
            if (fft_task_cycle_mean_sensor_ != nullptr)
                fft_task_cycle_mean_sensor_->publish_state(mean_us);
            if (fft_task_cycle_peak_sensor_ != nullptr)
                fft_task_cycle_peak_sensor_->publish_state(static_cast<float>(fft_cycle_peak_us_));
        }
        fft_cycle_total_us_ = 0;
        fft_cycle_peak_us_ = 0;
        fft_cycle_samples_ = 0;
        fft_cycle_last_publish_ms_ = now;
    }

    if (muted_) return;
    if ((now - last_process_ms_) < update_interval_ms_) return;

    // Read latest FFT results from double-buffer (lock-free)
    if (!new_data_available_) return;
    new_data_available_ = false;
    int read_slot = shared_write_idx_;
    BandEnergies16 energies = shared_frames_[read_slot].energies;
#ifndef AUDIO_REACTIVE_PRO
    float complex_onset = shared_frames_[read_slot].complex_onset;
#endif

#ifdef AUDIO_REACTIVE_PRO
    // Snapshot mel frame + SuperFlux + BTrack outputs from the same read slot (copy
    // is cheap at N_MEL=32) so the 7-band aggregation, onset publish, and beat publish
    // all see a consistent frame. Reading individual fields after this point risks
    // tearing if the FFT task re-publishes mid-loop.
    float mel_frame_snapshot[N_MEL];
    std::memcpy(mel_frame_snapshot, shared_frames_[read_slot].mel_frame, sizeof(mel_frame_snapshot));
    float sf_strength = shared_frames_[read_slot].superflux_strength;
    bool sf_event = shared_frames_[read_slot].superflux_event;
    float bt_bpm = shared_frames_[read_slot].btrack_bpm;
    float bt_phase = shared_frames_[read_slot].btrack_phase;
    float bt_confidence = shared_frames_[read_slot].btrack_confidence;
    bool bt_event = shared_frames_[read_slot].btrack_event;
    uint32_t frame_id = shared_frames_[read_slot].frame_id;
    // frame-id guard: main loop (~50Hz) consumes FFT frames (~86Hz) at a slower
    // cadence, so ~1 in 2 frames is naturally dropped. The beat_event publish
    // below checks frame_id != last_consumed_frame_id_ to avoid double-publishing
    // the same event when the main loop runs twice before the FFT task advances.
    bool new_frame = (frame_id != last_consumed_frame_id_);
    last_consumed_frame_id_ = frame_id;
#endif

    last_process_ms_ = now;

    if (debug_logging_) process_debug_logging_(now, energies);
    accumulate_calibration_(now, energies);

#ifdef AUDIO_REACTIVE_PRO
    // Pro-tier quiet-calibration accumulator: capture per-musical-band raw mel
    // sums in the same domain the AGCs see. MUST run before the silence-path
    // early-return below — quiet calibration is BY DEFINITION run in a silent
    // room, so the silence detector trips and would skip this block if it sat
    // after the early return. (That bug zeroed every cal_sum_mel_band_ entry,
    // produced noise_floor=0 across all 7 bands, and caused AGC wind-up
    // saturation during silence.)
    if (cal_state_ == CAL_QUIET) {
        for (uint8_t m = 0; m < MusicalBands::kNumBands; m++) {
            float band_sum = 0.0f;
            for (uint8_t i = MusicalBands::kMelStart[m]; i < MusicalBands::kMelEnd[m]; i++) {
                band_sum += mel_frame_snapshot[i];
            }
            cal_sum_mel_band_[m] += band_sum;
        }
    }
#endif

    // Silence detection on mid+high energy
    float silence_signal = energies.mid + energies.high;
    auto silence_result = silence_det_.update(silence_signal, now);

    if (silence_result.is_below_gate) {
        agc_bass_.suspend();
        agc_mid_.suspend();
        agc_high_.suspend();
        agc_amp_.suspend();
        publish_zeros_();

        if (silence_result.is_silent && !prev_silence_) {
            if (bpm_sensor_ != nullptr) bpm_sensor_->publish_state(0.0f);
            if (beat_confidence_sensor_ != nullptr) beat_confidence_sensor_->publish_state(0.0f);
            if (beat_phase_sensor_ != nullptr) beat_phase_sensor_->publish_state(0.0f);
            // onset_strength is event-driven (only published when an onset fires),
            // so without an explicit zero on the silence edge it would freeze at
            // the last event's value indefinitely.
            if (onset_strength_sensor_ != nullptr) onset_strength_sensor_->publish_state(0.0f);
            if (onset_det_ != nullptr) onset_det_->reset();
            if (beat_tracker_ != nullptr) beat_tracker_->reset();
#ifdef AUDIO_REACTIVE_PRO
            superflux_.reset();
            btrack_.reset();
#endif
        }
        if (silence_result.is_silent != prev_silence_) {
            if (silence_sensor_ != nullptr) silence_sensor_->publish_state(silence_result.is_silent);
            on_silence_changed_callbacks_.call();
        }
        prev_silence_ = silence_result.is_silent;
        return;
    }

    if (prev_silence_) {
        if (silence_sensor_ != nullptr) silence_sensor_->publish_state(false);
        on_silence_changed_callbacks_.call();
    }
    prev_silence_ = false;

    // DSP chain: pre-scale → AGC → limiter → EMA smoothing
    float scaled_bass = energies.bass * raw_scale_;
    float scaled_mid = energies.mid * raw_scale_;
    float scaled_high = energies.high * raw_scale_;
    float scaled_amp = energies.amplitude * raw_scale_;

    smooth_bass_ = asymmetric_ema(agc_bass_.process(scaled_bass), smooth_bass_);
    smooth_mid_ = asymmetric_ema(agc_mid_.process(scaled_mid), smooth_mid_);
    smooth_high_ = asymmetric_ema(agc_high_.process(scaled_high), smooth_high_);
    smooth_amp_ = asymmetric_ema(
        limiter_.process(agc_amp_.process(scaled_amp), static_cast<float>(update_interval_ms_)),
        smooth_amp_);

    // Onset detection + beat tracking: basic-tier only. Pro tier uses SuperFlux
    // and BTrack (both written by the FFT task).
    OnsetDetector::OnsetResult onset_result{};
#ifndef AUDIO_REACTIVE_PRO
    float scaled_bands[16];
    for (int i = 0; i < 16; i++) scaled_bands[i] = energies.bands[i] * raw_scale_;
    onset_result = onset_det_->update(scaled_bands, smooth_bass_, now, complex_onset);
    if (beat_tracker_ != nullptr) beat_tracker_->process(onset_det_->last_onset_value());
#endif

#ifdef AUDIO_REACTIVE_PRO
    // Pro tier: compute 7 musical bands from the raw mel frame. Each band applies
    // per-band AGC + asymmetric EMA smoothing internally.
    // (Quiet-calibration accumulator was moved above, before the silence early-
    // return path, so it runs even during the silent calibration window.)
    musical_bands_.process(mel_frame_snapshot, musical_band_energies_);

    // Pro-tier debug-logging instrumentation. Populates per-iteration metrics
    // for the 2-second log dump in process_debug_logging_(). Cheap when
    // debug_logging_ is false (single bool branch). When enabled it records
    // raw mel sums (in the AGC's input domain), band-sensor min/max ranges,
    // BTrack and SuperFlux state — enough to verify the AGC saturation and
    // BPM-lock hypotheses on real hardware without device-side instrumentation.
    if (debug_logging_) {
        for (uint8_t b = 0; b < MusicalBands::kNumBands; b++) {
            float raw_sum = 0.0f;
            for (uint8_t i = MusicalBands::kMelStart[b]; i < MusicalBands::kMelEnd[b]; i++) {
                raw_sum += mel_frame_snapshot[i];
            }
            pro_debug_metrics_.raw_mel_sum[b] = raw_sum;
            float v = musical_band_energies_[b];
            if (v > pro_debug_metrics_.band_max[b]) pro_debug_metrics_.band_max[b] = v;
            if (v < pro_debug_metrics_.band_min[b] || pro_debug_metrics_.band_min[b] == 0.0f) {
                pro_debug_metrics_.band_min[b] = v;
            }
        }
        pro_debug_metrics_.btrack_bpm = bt_bpm;
        pro_debug_metrics_.btrack_confidence = bt_confidence;
        if (sf_strength > pro_debug_metrics_.superflux_strength_max) {
            pro_debug_metrics_.superflux_strength_max = sf_strength;
        }
    }

    // SuperFlux onset event: publish on event only (matches basic-tier cadence).
    if (sf_event) {
        if (onset_strength_sensor_ != nullptr) onset_strength_sensor_->publish_state(sf_strength);
        if (onset_sensor_ != nullptr) onset_sensor_->publish_state(true);
        onset_on_ms_ = now;
    }

    // BTrack publish: BPM cadence-limited (every 3s like basic-tier), phase/confidence
    // every loop iteration, beat_event pulsed for 30ms on a new frame only.
    // BPM publish is gated on confidence: BTrack preserves current_bpm_ across
    // silence/no-signal windows, so an un-gated publish would expose a stale
    // value as if it were a live tempo. Below kSilenceConfidence we publish 0
    // to mark the sensor as "no tempo lock".
    if ((now - last_bpm_publish_ms_) >= BPM_PUBLISH_INTERVAL_MS) {
        if (bpm_sensor_ != nullptr) {
            float reported_bpm = (bt_confidence >= BTrack::kSilenceConfidence) ? bt_bpm : 0.0f;
            bpm_sensor_->publish_state(reported_bpm);
        }
        last_bpm_publish_ms_ = now;
    }
    if (beat_phase_sensor_ != nullptr) beat_phase_sensor_->publish_state(bt_phase);
    if (beat_confidence_sensor_ != nullptr) beat_confidence_sensor_->publish_state(bt_confidence);
    if (new_frame && beat_event_sensor_ != nullptr && bt_event) {
        beat_event_sensor_->publish_state(true);
        beat_event_off_at_ms_ = now + 30;  // 30ms pulse
    }
    if (beat_event_sensor_ != nullptr && beat_event_off_at_ms_ > 0 && now >= beat_event_off_at_ms_) {
        beat_event_sensor_->publish_state(false);
        beat_event_off_at_ms_ = 0;
    }
#endif

    publish_sensor_values_(now, energies, onset_result);
}

void AudioReactiveComponent::process_debug_logging_(uint32_t now, const BandEnergies16 &energies) {
    static uint32_t last_debug_ms = 0;
    static float raw_amp_min = 1e10f, raw_amp_max = 0.0f;
    static float raw_bass_min = 1e10f, raw_bass_max = 0.0f;

    if (energies.amplitude < raw_amp_min) raw_amp_min = energies.amplitude;
    if (energies.amplitude > raw_amp_max) raw_amp_max = energies.amplitude;
    if (energies.bass < raw_bass_min) raw_bass_min = energies.bass;
    if (energies.bass > raw_bass_max) raw_bass_max = energies.bass;

    if ((now - last_debug_ms) < 2000) return;

    ESP_LOGI(TAG, "=== AUDIO DEBUG === (sample_rate=%.0f, fft_size=%u)", sample_rate_, FFT_SIZE);
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
    ESP_LOGI(TAG, "Silence signal (mid+high)=%.2f | Squelch=%.1f (user_thresh=%.2f, eff_thresh=%.2f, cal_thresh=%.2f)",
             dbg_silence_signal, silence_det_.squelch(), silence_det_.squelch() * 0.5f,
             silence_det_.effective_threshold(), cal_store_.squelch_threshold);
    ESP_LOGI(TAG, "Silence state: prev_silence=%d", prev_silence_);
    ESP_LOGI(TAG, "Calibration: quiet=%s music=%s scale=%.4f squelch_thresh=%.2f",
             cal_store_.quiet_calibrated ? "yes" : "no",
             cal_store_.music_calibrated ? "yes" : "no",
             cal_store_.raw_scale, cal_store_.squelch_threshold);
    ESP_LOGI(TAG, "Published: bass=%.3f mid=%.3f high=%.3f amp=%.3f",
             smooth_bass_, smooth_mid_, smooth_high_, smooth_amp_);
    ESP_LOGI(TAG, "Ring buffer: %u/%u samples, dropped=%u",
             static_cast<unsigned>(ring_buffer_.available()),
             static_cast<unsigned>(ring_buffer_.capacity()),
             static_cast<unsigned>(ring_dropped_samples_));
    // Reset so each dump reports drops per interval. (When debug logging is
    // disabled the counter accumulates silently until the next dump - fine
    // for a diagnostic.)
    ring_dropped_samples_ = 0;

    raw_amp_min = 1e10f; raw_amp_max = 0.0f;
    raw_bass_min = 1e10f; raw_bass_max = 0.0f;

#ifdef AUDIO_REACTIVE_PRO
    ESP_LOGI(TAG, "=== PRO DEBUG ===");
    ESP_LOGI(TAG, "Raw mel sums: low_bass=%.5f bass=%.5f low_mid=%.5f mid=%.5f upper_mid=%.5f high=%.5f air=%.5f",
             pro_debug_metrics_.raw_mel_sum[0], pro_debug_metrics_.raw_mel_sum[1],
             pro_debug_metrics_.raw_mel_sum[2], pro_debug_metrics_.raw_mel_sum[3],
             pro_debug_metrics_.raw_mel_sum[4], pro_debug_metrics_.raw_mel_sum[5],
             pro_debug_metrics_.raw_mel_sum[6]);
    ESP_LOGI(TAG, "Band sensors (2s range): low_bass=[%.3f..%.3f] bass=[%.3f..%.3f] low_mid=[%.3f..%.3f] mid=[%.3f..%.3f] upper_mid=[%.3f..%.3f] high=[%.3f..%.3f] air=[%.3f..%.3f]",
             pro_debug_metrics_.band_min[0], pro_debug_metrics_.band_max[0],
             pro_debug_metrics_.band_min[1], pro_debug_metrics_.band_max[1],
             pro_debug_metrics_.band_min[2], pro_debug_metrics_.band_max[2],
             pro_debug_metrics_.band_min[3], pro_debug_metrics_.band_max[3],
             pro_debug_metrics_.band_min[4], pro_debug_metrics_.band_max[4],
             pro_debug_metrics_.band_min[5], pro_debug_metrics_.band_max[5],
             pro_debug_metrics_.band_min[6], pro_debug_metrics_.band_max[6]);
    ESP_LOGI(TAG, "Per-band AGC gain: low_bass=%.2f bass=%.2f low_mid=%.2f mid=%.2f upper_mid=%.2f high=%.2f air=%.2f",
             musical_bands_.agc(0).current_gain(), musical_bands_.agc(1).current_gain(),
             musical_bands_.agc(2).current_gain(), musical_bands_.agc(3).current_gain(),
             musical_bands_.agc(4).current_gain(), musical_bands_.agc(5).current_gain(),
             musical_bands_.agc(6).current_gain());
    ESP_LOGI(TAG, "V2 noise floors: low_bass=%.4f bass=%.4f low_mid=%.4f mid=%.4f upper_mid=%.4f high=%.4f air=%.4f",
             cal_store_v2_.noise_floor[0], cal_store_v2_.noise_floor[1],
             cal_store_v2_.noise_floor[2], cal_store_v2_.noise_floor[3],
             cal_store_v2_.noise_floor[4], cal_store_v2_.noise_floor[5],
             cal_store_v2_.noise_floor[6]);
    ESP_LOGI(TAG, "BTrack: bpm=%.1f confidence=%.3f | SuperFlux peak strength (2s): %.4f",
             pro_debug_metrics_.btrack_bpm, pro_debug_metrics_.btrack_confidence,
             pro_debug_metrics_.superflux_strength_max);
    // Reset the 2-second-window aggregates for the next interval.
    for (uint8_t b = 0; b < MusicalBands::kNumBands; b++) {
        pro_debug_metrics_.band_min[b] = 0.0f;
        pro_debug_metrics_.band_max[b] = 0.0f;
    }
    pro_debug_metrics_.superflux_strength_max = 0.0f;
#endif

    last_debug_ms = now;
}

void AudioReactiveComponent::accumulate_calibration_(uint32_t now, const BandEnergies16 &energies) {
    if (cal_state_ == CAL_IDLE) return;
    uint32_t elapsed = now - cal_start_ms_;

    if (cal_state_ == CAL_QUIET) {
        cal_max_mid_high_ = std::max(cal_max_mid_high_, energies.mid + energies.high);
        cal_max_bass_ = std::max(cal_max_bass_, energies.bass);
        cal_max_mid_ = std::max(cal_max_mid_, energies.mid);
        cal_max_high_ = std::max(cal_max_high_, energies.high);
        cal_max_amp_ = std::max(cal_max_amp_, energies.amplitude);
        cal_sum_bass_ += energies.bass;
        cal_sum_mid_ += energies.mid;
        cal_sum_high_ += energies.high;
        cal_sum_amp_quiet_ += energies.amplitude;
        cal_quiet_count_++;
        if (elapsed >= 3000) finish_quiet_calibration();
    } else if (cal_state_ == CAL_MUSIC) {
        cal_sum_amp_ += energies.amplitude;
        cal_max_bass_ = std::max(cal_max_bass_, energies.bass);
        cal_max_mid_ = std::max(cal_max_mid_, energies.mid);
        cal_max_high_ = std::max(cal_max_high_, energies.high);
        cal_sample_count_++;
        if (elapsed >= 5000) finish_music_calibration();
    }
}

void AudioReactiveComponent::publish_sensor_values_(uint32_t now, const BandEnergies16 &energies,
                                                     const OnsetDetector::OnsetResult &onset_result) {
#ifndef AUDIO_REACTIVE_PRO
    // Basic tier: publish bass/mid/high from band_aggregator output via smoothed EMA.
    if (bass_sensor_ != nullptr) bass_sensor_->publish_state(smooth_bass_);
    if (mid_sensor_ != nullptr) mid_sensor_->publish_state(smooth_mid_);
    if (high_sensor_ != nullptr) high_sensor_->publish_state(smooth_high_);
#else
    // Pro tier: publish 7 musical bands (bass/mid/high override basic-tier values).
    // musical_band_energies_ is populated in loop() from the shared mel frame.
    if (sub_bass_sensor_ != nullptr) sub_bass_sensor_->publish_state(musical_band_energies_[MusicalBands::SUB_BASS]);
    if (bass_sensor_ != nullptr) bass_sensor_->publish_state(musical_band_energies_[MusicalBands::BASS]);
    if (low_mid_sensor_ != nullptr) low_mid_sensor_->publish_state(musical_band_energies_[MusicalBands::LOW_MID]);
    if (mid_sensor_ != nullptr) mid_sensor_->publish_state(musical_band_energies_[MusicalBands::MID]);
    if (upper_mid_sensor_ != nullptr) upper_mid_sensor_->publish_state(musical_band_energies_[MusicalBands::UPPER_MID]);
    if (high_sensor_ != nullptr) high_sensor_->publish_state(musical_band_energies_[MusicalBands::HIGH]);
    if (air_sensor_ != nullptr) air_sensor_->publish_state(musical_band_energies_[MusicalBands::AIR]);
#endif
    if (amplitude_sensor_ != nullptr) amplitude_sensor_->publish_state(smooth_amp_);

    if (centroid_sensor_ != nullptr) {
        float nyquist = sample_rate_ / 2.0f;
        centroid_sensor_->publish_state((nyquist > 0.0f) ? (energies.centroid / nyquist) : 0.0f);
    }
    if (rolloff_sensor_ != nullptr) {
        float nyquist = sample_rate_ / 2.0f;
        rolloff_sensor_->publish_state((nyquist > 0.0f) ? (energies.rolloff / nyquist) : 0.0f);
    }

#ifndef AUDIO_REACTIVE_PRO
    if (onset_result.detected) {
        if (onset_sensor_ != nullptr) { onset_sensor_->publish_state(true); onset_on_ms_ = now; }
        if (onset_strength_sensor_ != nullptr) onset_strength_sensor_->publish_state(onset_result.strength);
    }

    if ((now - last_bpm_publish_ms_) >= BPM_PUBLISH_INTERVAL_MS) {
        if (beat_tracker_ != nullptr) {
            auto bt = beat_tracker_->result();
            if (bpm_sensor_ != nullptr) bpm_sensor_->publish_state(bt.bpm);
            if (beat_confidence_sensor_ != nullptr) beat_confidence_sensor_->publish_state(bt.confidence);
            if (beat_phase_sensor_ != nullptr) beat_phase_sensor_->publish_state(bt.phase);
        }
        last_bpm_publish_ms_ = now;
    }
#else
    // Pro tier: SuperFlux event-driven onset publish happens in loop() before this
    // function runs. BPM / confidence / phase sensors are published by the BTrack
    // block in loop() as well, so this function is only responsible for the
    // basic-tier legacy publish path.
    (void)onset_result;
#endif
}

void AudioReactiveComponent::publish_zeros_() {
    if (bass_sensor_ != nullptr) bass_sensor_->publish_state(0.0f);
    if (mid_sensor_ != nullptr) mid_sensor_->publish_state(0.0f);
    if (high_sensor_ != nullptr) high_sensor_->publish_state(0.0f);
    if (amplitude_sensor_ != nullptr) amplitude_sensor_->publish_state(0.0f);
    if (centroid_sensor_ != nullptr) centroid_sensor_->publish_state(0.0f);
    if (rolloff_sensor_ != nullptr) rolloff_sensor_->publish_state(0.0f);
#ifdef AUDIO_REACTIVE_PRO
    // Pro-tier musical-band sensors: also zero on silence so they don't freeze
    // at the last non-silent value. The pointer fields are #ifdef-gated in
    // the header, so this block must be too.
    if (sub_bass_sensor_ != nullptr) sub_bass_sensor_->publish_state(0.0f);
    if (low_mid_sensor_ != nullptr) low_mid_sensor_->publish_state(0.0f);
    if (upper_mid_sensor_ != nullptr) upper_mid_sensor_->publish_state(0.0f);
    if (air_sensor_ != nullptr) air_sensor_->publish_state(0.0f);
#endif
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
#ifndef AUDIO_REACTIVE_PRO
        has_prev_frame_ = false;
#endif
        whitening_.reset();

        // Publish zeros for all sensors including BPM
        publish_zeros_();
        if (bpm_sensor_ != nullptr) bpm_sensor_->publish_state(0.0f);
        if (onset_det_ != nullptr) onset_det_->reset();
#ifdef AUDIO_REACTIVE_PRO
        superflux_.reset();
        btrack_.reset();
#endif

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

void AudioReactiveComponent::reset_agc() {
    ESP_LOGI(TAG, "Resetting AGC and onset detector");
    agc_bass_.reset();
    agc_mid_.reset();
    agc_high_.reset();
    agc_amp_.reset();
    whitening_.reset();
    if (onset_det_ != nullptr) {
        onset_det_->reset();
    }
    if (beat_tracker_ != nullptr) {
        beat_tracker_->reset();
    }
#ifdef AUDIO_REACTIVE_PRO
    superflux_.reset();
    btrack_.reset();
#endif
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
    parent_->set_squelch(value);
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
    } else if (value == "complex_domain") {
        parent_->onset_det_->set_mode(OnsetDetector::MODE_COMPLEX_DOMAIN);
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
    cal_sum_bass_ = 0;
    cal_sum_mid_ = 0;
    cal_sum_high_ = 0;
    cal_sum_amp_quiet_ = 0;
    cal_quiet_count_ = 0;
#ifdef AUDIO_REACTIVE_PRO
    for (uint8_t m = 0; m < MusicalBands::kNumBands; m++) cal_sum_mel_band_[m] = 0.0f;
#endif
    ESP_LOGI(TAG, "Quiet room calibration started (3 seconds)...");
    on_quiet_calibration_started_callbacks_.call();
}

void AudioReactiveComponent::finish_quiet_calibration() {
    cal_state_ = CAL_IDLE;

    // Compute thresholds
    cal_store_.squelch_threshold = cal_max_mid_high_ * 1.5f;
    float scale = cal_store_.music_calibrated ? cal_store_.raw_scale : (1.0f / 20.0f);
    // Use MEAN (not max) for per-band noise floors.
    // Max captures AC hum spikes which are much higher than actual noise baseline,
    // causing the AGC to suppress all bass on PDM mics with strong low-frequency noise.
    if (cal_quiet_count_ > 0) {
        float mean_bass = cal_sum_bass_ / cal_quiet_count_;
        float mean_mid = cal_sum_mid_ / cal_quiet_count_;
        float mean_high = cal_sum_high_ / cal_quiet_count_;
        float mean_amp = cal_sum_amp_quiet_ / cal_quiet_count_;
        cal_store_.noise_floor_bass = (mean_bass * scale) * 1.5f;  // 50% headroom above mean
        cal_store_.noise_floor_mid = (mean_mid * scale) * 1.5f;
        cal_store_.noise_floor_high = (mean_high * scale) * 1.5f;
        cal_store_.noise_floor_amp = (mean_amp * scale) * 1.5f;
    } else {
        cal_store_.noise_floor_bass = 0.25f;
        cal_store_.noise_floor_mid = 0.08f;
        cal_store_.noise_floor_high = 0.03f;
        cal_store_.noise_floor_amp = 0.10f;
    }
    cal_store_.quiet_calibrated = true;

#ifdef AUDIO_REACTIVE_PRO
    // Pro tier: compute V2 noise floors directly from the per-musical-band sums
    // accumulated during the quiet window. These are in the same domain the
    // musical_bands_ AGCs see (raw mel-slot sums, un-scaled by raw_scale_),
    // so floor = mean × 1.5 gives a correct per-band suppression threshold.
    // The V1->V2 interpolation path is reserved for the boot-time fallback when
    // only a legacy V1 NVS store exists; a fresh quiet calibration always uses
    // direct per-band measurement.
    if (cal_quiet_count_ > 0) {
        // Defensive check: if every band sum is zero the accumulator never ran
        // (e.g., the silence early-return path skipped it), and writing all-zero
        // noise floors would defeat the AGC suppression and cause wind-up
        // saturation at runtime. Surface the failure via WARN and fall back to
        // the V1->V2 interpolated floors so the device still behaves sensibly.
        float total_sum = 0.0f;
        for (uint8_t m = 0; m < MusicalBands::kNumBands; m++) total_sum += cal_sum_mel_band_[m];
        if (total_sum <= 0.0f) {
            ESP_LOGW(TAG, "Pro-tier mel-band accumulator captured zero data during quiet "
                          "calibration - falling back to V1->V2 interpolation. Likely cause: "
                          "the accumulator was placed below the silence-path early-return.");
            migrate_v1_to_v2(cal_store_, cal_store_v2_);
        } else {
            for (uint8_t m = 0; m < MusicalBands::kNumBands; m++) {
                float mean = cal_sum_mel_band_[m] / static_cast<float>(cal_quiet_count_);
                cal_store_v2_.noise_floor[m] = mean * 1.5f;
            }
        }
    }
    // squelch_threshold is consumed by silence_det_ on basic-tier energies
    // (mid + high), so the V1-domain value is the right scale here.
    cal_store_v2_.squelch_threshold = cal_store_.squelch_threshold;
    cal_store_v2_.quiet_calibrated = true;

    // apply_calibration_v2_() pushes V2 floors into musical_bands_ AND calls
    // apply_calibration() to sync basic-tier scalar state. Single call replaces
    // the basic-tier-only apply path used for non-pro builds.
    apply_calibration_v2_();
    cal_pref_.save(&cal_store_);
    cal_pref_v2_.save(&cal_store_v2_);
    cal_stale_ = false;
    if (calibration_stale_sensor_ != nullptr) {
        calibration_stale_sensor_->publish_state(false);
    }
    ESP_LOGI(TAG, "V2 noise floors: low_bass=%.3f bass=%.3f low_mid=%.3f mid=%.3f upper_mid=%.3f high=%.3f air=%.3f",
             cal_store_v2_.noise_floor[0], cal_store_v2_.noise_floor[1],
             cal_store_v2_.noise_floor[2], cal_store_v2_.noise_floor[3],
             cal_store_v2_.noise_floor[4], cal_store_v2_.noise_floor[5],
             cal_store_v2_.noise_floor[6]);
#else
    apply_calibration();
    cal_pref_.save(&cal_store_);
#endif

    ESP_LOGI(TAG, "Quiet calibration done: squelch_threshold=%.2f, noise_floors: bass=%.3f mid=%.3f high=%.3f amp=%.3f",
             cal_store_.squelch_threshold, cal_store_.noise_floor_bass,
             cal_store_.noise_floor_mid, cal_store_.noise_floor_high, cal_store_.noise_floor_amp);
    on_quiet_calibration_complete_callbacks_.call();
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
    on_music_calibration_started_callbacks_.call();
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

#ifdef AUDIO_REACTIVE_PRO
        cal_store_v2_.raw_scale = cal_store_.raw_scale;
        cal_store_v2_.music_calibrated = true;
        cal_pref_v2_.save(&cal_store_v2_);
        cal_stale_ = false;
        if (calibration_stale_sensor_ != nullptr) {
            calibration_stale_sensor_->publish_state(false);
        }
#endif

        ESP_LOGI(TAG, "Music calibration done: avg_amp=%.2f, raw_scale=%.4f",
                 avg_amp, cal_store_.raw_scale);
        on_music_calibration_complete_callbacks_.call();
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

#ifdef AUDIO_REACTIVE_PRO
void AudioReactiveComponent::apply_calibration_v2_() {
    // Push per-musical-band noise floors directly into the 7 musical_bands AGCs.
    musical_bands_.set_noise_floors(cal_store_v2_.noise_floor);
    // Reset musical_bands_ AGC state (gain / integrator / sample tracking /
    // smoothed_) so post-calibration behavior reflects the new floors without
    // any wind-up that may have accumulated under stale/wrong floors. The
    // PI loop converges on the new equilibrium from defaults rather than from
    // a possibly-saturated prior state.
    musical_bands_.reset();
    // Also invoke the basic-tier apply path so shared scalar state (raw_scale_,
    // squelch threshold, basic-tier AGC floors mirrored from V2) stays in sync.
    apply_calibration();
}
#endif  // AUDIO_REACTIVE_PRO

void AudioReactiveCalibrateQuietButton::press_action() {
    parent_->start_quiet_calibration();
}

void AudioReactiveCalibrateMusicButton::press_action() {
    parent_->start_music_calibration();
}

void AudioReactiveComponent::dump_config() {
    ESP_LOGCONFIG(TAG, "AudioReactive:");
    ESP_LOGCONFIG(TAG, "  Sample rate: %.0f Hz", sample_rate_);
    ESP_LOGCONFIG(TAG, "  FFT size: %u", FFT_SIZE);
    ESP_LOGCONFIG(TAG, "  Hop size: %u", HOP_SIZE);
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
