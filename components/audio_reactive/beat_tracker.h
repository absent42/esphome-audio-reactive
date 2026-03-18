#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome {
namespace audio_reactive {

class BeatTracker {
 public:
    struct Result {
        float bpm;          // 0 if unknown
        float confidence;   // 0.0-1.0
        float phase;        // 0.0-1.0, position within current beat period
    };

    explicit BeatTracker(float update_rate_hz)
        : update_rate_(update_rate_hz),
          write_pos_(0),
          frames_written_(0),
          bp_(0.0f),
          confidence_(0.0f),
          phase_(0.0f) {
        // Window: ~4 seconds of history, capped at MAX_WIN
        size_t desired_win = static_cast<size_t>(update_rate_hz * 4.0f);
        win_len_ = (desired_win < MAX_WIN) ? desired_win : MAX_WIN;
        lag_len_ = win_len_ / 2;
        if (lag_len_ > MAX_LAG) lag_len_ = MAX_LAG;

        // Rayleigh weighting biased toward 120 BPM
        // rp = preferred period in frames
        float rp = update_rate_hz * 60.0f / 120.0f;
        for (size_t i = 0; i < MAX_LAG; i++) {
            float x = static_cast<float>(i + 1);
            rayleigh_[i] = (x / (rp * rp)) * std::exp(-0.5f * x * x / (rp * rp));
        }

        std::memset(df_buffer_, 0, sizeof(df_buffer_));
        std::memset(acf_, 0, sizeof(acf_));
        std::memset(acf_out_, 0, sizeof(acf_out_));
    }

    void process(float onset_value) {
        df_buffer_[write_pos_] = onset_value;
        write_pos_ = (write_pos_ + 1) % win_len_;
        frames_written_++;

        // Only run estimation once we have a full window, every win_len/4 frames
        size_t quarter = win_len_ / 4;
        if (quarter == 0) quarter = 1;
        if (frames_written_ >= win_len_ && (frames_written_ % quarter) == 0) {
            compute_autocorrelation_();
            apply_comb_filterbank_();
            estimate_period_();
            estimate_phase_();
        }

        // Always update phase by advancing it
        if (bp_ > 0.0f && confidence_ > 0.0f) {
            phase_ += 1.0f / bp_;
            if (phase_ >= 1.0f) phase_ -= 1.0f;
        }
    }

    Result result() const {
        Result r;
        r.bpm = (bp_ > 0.0f && confidence_ > 0.0f) ? (update_rate_ * 60.0f / bp_) : 0.0f;
        r.confidence = confidence_;
        r.phase = phase_;
        return r;
    }

    void reset() {
        write_pos_ = 0;
        frames_written_ = 0;
        bp_ = 0.0f;
        confidence_ = 0.0f;
        phase_ = 0.0f;
        std::memset(df_buffer_, 0, sizeof(df_buffer_));
        std::memset(acf_, 0, sizeof(acf_));
        std::memset(acf_out_, 0, sizeof(acf_out_));
    }

 private:
    static constexpr size_t MAX_WIN = 128;   // ~6.4s at 20Hz
    static constexpr size_t MAX_LAG = 64;    // ~3.2s at 20Hz → min ~18.75 BPM

    float update_rate_;
    size_t win_len_;
    size_t lag_len_;
    size_t write_pos_;
    size_t frames_written_;

    float df_buffer_[MAX_WIN];
    float acf_[MAX_WIN];
    float acf_out_[MAX_LAG];
    float rayleigh_[MAX_LAG];

    float bp_;
    float confidence_;
    float phase_;

    float df_read_(size_t linear_idx) const {
        // linear_idx 0 = oldest sample
        size_t idx = (write_pos_ + linear_idx) % win_len_;
        return df_buffer_[idx];
    }

    void compute_autocorrelation_() {
        // Unnormalized ACF of the linearized buffer
        for (size_t lag = 0; lag < win_len_; lag++) {
            float sum = 0.0f;
            size_t count = win_len_ - lag;
            for (size_t n = 0; n < count; n++) {
                sum += df_read_(n) * df_read_(n + lag);
            }
            acf_[lag] = sum;
        }
    }

    void apply_comb_filterbank_() {
        // 4 harmonics with sub-harmonic interpolation, then Rayleigh weighting
        for (size_t i = 1; i < lag_len_ - 1; i++) {
            float val = 0.0f;
            for (int a = 1; a <= 4; a++) {
                float harmonic_sum = 0.0f;
                int count = 2 * a - 1;
                for (int b = 1; b <= count; b++) {
                    size_t idx = static_cast<size_t>(i * a + b - 1);
                    if (idx < win_len_) {
                        harmonic_sum += acf_[idx];
                    }
                }
                val += harmonic_sum / static_cast<float>(count);
            }
            acf_out_[i] = val * rayleigh_[i];
        }
        // Zero out boundaries
        if (lag_len_ > 0) acf_out_[0] = 0.0f;
        if (lag_len_ > 1) acf_out_[lag_len_ - 1] = 0.0f;
    }

    void estimate_period_() {
        if (lag_len_ < 3) return;

        // Find maximum in acf_out (skip first and last)
        float max_val = 0.0f;
        size_t max_idx = 1;
        for (size_t i = 1; i < lag_len_ - 1; i++) {
            if (acf_out_[i] > max_val) {
                max_val = acf_out_[i];
                max_idx = i;
            }
        }

        // Need a meaningful peak
        if (max_val <= 0.0f) {
            confidence_ = 0.0f;
            bp_ = 0.0f;
            return;
        }

        // Sub-sample refinement
        float refined_period = static_cast<float>(max_idx);
        if (max_idx > 0 && max_idx < lag_len_ - 1) {
            refined_period = quadratic_peak_pos_(max_idx);
        }

        // BPM from comb-filtered period, with range correction
        float bpm = update_rate_ * 60.0f / refined_period;
        while (bpm > 200.0f && refined_period > 1.0f) {
            refined_period *= 2.0f;
            bpm = update_rate_ * 60.0f / refined_period;
        }
        while (bpm < 40.0f) {
            refined_period /= 2.0f;
            if (refined_period <= 1.0f) break;
            bpm = update_rate_ * 60.0f / refined_period;
        }

        // Sub-harmonic protection: the comb filterbank biased toward 120 BPM may
        // latch onto a harmonic of the true (slower) period. Find the dominant raw
        // ACF peak in the full valid range and compare against the comb candidate.
        // If raw ACF has a much stronger peak at a longer period, prefer it.
        {
            size_t comb_idx = static_cast<size_t>(refined_period + 0.5f);
            float comb_acf = (comb_idx < win_len_) ? acf_[comb_idx] : 0.0f;

            // Search raw ACF for dominant peak in valid tempo range [30..220 BPM]
            size_t raw_lo = static_cast<size_t>(update_rate_ * 60.0f / 220.0f + 0.5f);
            size_t raw_hi = static_cast<size_t>(update_rate_ * 60.0f / 30.0f + 0.5f);
            if (raw_lo < 1) raw_lo = 1;
            if (raw_hi >= win_len_) raw_hi = win_len_ - 1;

            float raw_max = 0.0f;
            size_t raw_max_idx = comb_idx;
            for (size_t i = raw_lo; i <= raw_hi; i++) {
                if (acf_[i] > raw_max) {
                    raw_max = acf_[i];
                    raw_max_idx = i;
                }
            }

            // If the raw ACF peak is at a different lag and is >= 3x stronger,
            // switch to the raw peak period (it's the true dominant period)
            if (comb_acf > 0.0f && raw_max_idx != comb_idx &&
                raw_max / comb_acf >= 3.0f) {
                refined_period = static_cast<float>(raw_max_idx);
                bpm = update_rate_ * 60.0f / refined_period;
                // Re-apply BPM range guard
                while (bpm > 200.0f && refined_period > 1.0f) {
                    refined_period *= 2.0f;
                    bpm = update_rate_ * 60.0f / refined_period;
                }
                while (bpm < 40.0f) {
                    refined_period /= 2.0f;
                    if (refined_period <= 1.0f) break;
                    bpm = update_rate_ * 60.0f / refined_period;
                }
            }
        }

        bp_ = refined_period;

        // Confidence = peak / total energy, scaled
        float total = 0.0f;
        for (size_t i = 0; i < lag_len_; i++) {
            total += acf_out_[i];
        }
        if (total > 0.0f) {
            float raw = max_val / total * 3.0f;
            confidence_ = (raw < 1.0f) ? raw : 1.0f;
        } else {
            confidence_ = 0.0f;
        }
    }

    void estimate_phase_() {
        if (bp_ <= 0.0f || confidence_ <= 0.0f) return;

        size_t period_int = static_cast<size_t>(bp_ + 0.5f);
        if (period_int == 0) period_int = 1;

        float best_sum = -1.0f;
        size_t best_offset = 0;

        for (size_t offset = 0; offset < period_int; offset++) {
            float sum = 0.0f;
            // Sum df values at positions aligned to this offset
            for (size_t pos = offset; pos < win_len_; pos += period_int) {
                sum += df_read_(pos);
            }
            if (sum > best_sum) {
                best_sum = sum;
                best_offset = offset;
            }
        }

        // Phase = where in the beat cycle are we currently?
        // current position is write_pos_ (just wrote there before incrementing)
        // "now" in linear indexing = win_len_ - 1 (the most recent sample)
        size_t current_linear = win_len_ - 1;
        size_t dist = (current_linear >= best_offset)
                          ? (current_linear - best_offset) % period_int
                          : period_int - ((best_offset - current_linear) % period_int);
        phase_ = static_cast<float>(dist) / static_cast<float>(period_int);
        if (phase_ >= 1.0f) phase_ = 0.0f;
    }

    float quadratic_peak_pos_(size_t idx) const {
        float a = acf_out_[idx - 1];
        float b = acf_out_[idx];
        float c = acf_out_[idx + 1];
        float denom = 2.0f * (2.0f * b - a - c);
        if (std::fabs(denom) < 1e-8f) return static_cast<float>(idx);
        return static_cast<float>(idx) + (a - c) / denom;
    }
};

}  // namespace audio_reactive
}  // namespace esphome
