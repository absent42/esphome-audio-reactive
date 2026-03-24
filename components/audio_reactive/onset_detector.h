#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome {
namespace audio_reactive {

/// Onset detector supporting spectral flux and bass-energy modes with BPM tracking.
class OnsetDetector {
 public:
    enum Mode { MODE_SPECTRAL_FLUX, MODE_BASS_ENERGY, MODE_COMPLEX_DOMAIN };
    enum OnsetType { TYPE_BEAT, TYPE_ONSET };

    struct OnsetResult {
        bool detected;
        float strength;           // 0.1-1.0, floored at 0.1
        int dominant_band_index;  // 0-15
        const char *dominant_band;  // "bass", "mid", or "high"
        OnsetType type;           // TYPE_BEAT if bass-dominant, TYPE_ONSET otherwise
    };

    /// @param sensitivity      1-100 (higher = lower threshold = more detections)
    /// @param mode             Spectral flux or bass energy
    /// @param window_size      Rolling window size (~3s at 20Hz = 60 samples)
    /// @param min_interval_ms  Minimum ms between onsets (prevents flicker)
    OnsetDetector(int sensitivity = 50, Mode mode = MODE_SPECTRAL_FLUX,
                  size_t window_size = 60, uint32_t min_interval_ms = 150)
        : mode_(mode),
          window_size_(window_size > MAX_WINDOW ? MAX_WINDOW : window_size),
          min_interval_ms_(min_interval_ms),
          last_onset_ms_(0),
          hysteresis_armed_(true),
          flux_count_(0),
          flux_head_(0),
          flux_sum_(0.0f),
          flux_sq_sum_(0.0f),
          interval_count_(0),
          interval_head_(0) {
        set_sensitivity(sensitivity);
        for (int i = 0; i < 16; i++) prev_bands_[i] = 0.0f;
        std::memset(flux_ring_, 0, sizeof(flux_ring_));
        std::memset(interval_ring_, 0, sizeof(interval_ring_));
    }

    /// Process a new frame of band energies.
    OnsetResult update(const float bands[16], float bass_energy, uint32_t timestamp_ms,
                       float external_onset_value = -1.0f) {
        OnsetResult result{false, 0.0f, 0, "bass", TYPE_ONSET};

        // Save snapshot of previous bands for dominant-band search below,
        // then compute flux (which reads prev_bands_) before overwriting.
        float saved_prev[16];
        for (int i = 0; i < 16; i++) saved_prev[i] = prev_bands_[i];

        float value;
        if (mode_ == MODE_COMPLEX_DOMAIN && external_onset_value >= 0.0f) {
            value = external_onset_value;
        } else if (mode_ == MODE_SPECTRAL_FLUX) {
            value = compute_spectral_flux(bands);
        } else {
            value = bass_energy;
        }
        last_value_ = value;

        // Update rolling window (circular array with incremental stats)
        if (flux_count_ >= window_size_) {
            // Evict oldest value from running sums
            float evicted = flux_ring_[flux_head_];
            flux_sum_ -= evicted;
            flux_sq_sum_ -= evicted * evicted;
        }
        flux_ring_[flux_head_] = value;
        flux_sum_ += value;
        flux_sq_sum_ += value * value;
        flux_head_ = (flux_head_ + 1) % window_size_;
        if (flux_count_ < window_size_) flux_count_++;

        // Update previous bands for next frame's flux computation
        for (int i = 0; i < 16; i++) prev_bands_[i] = bands[i];

        float threshold = compute_threshold();

        // Enforce minimum interval
        bool interval_ok = (last_onset_ms_ == 0) ||
                           ((timestamp_ms - last_onset_ms_) >= min_interval_ms_);

        bool triggered = false;
        if (mode_ == MODE_BASS_ENERGY) {
            // Hysteresis: must drop below 70% of threshold to re-arm
            if (value > threshold && interval_ok && hysteresis_armed_) {
                triggered = true;
                hysteresis_armed_ = false;
            } else if (value < threshold * 0.7f) {
                hysteresis_armed_ = true;
            }
        } else {
            if (value > threshold && interval_ok) {
                triggered = true;
            }
        }

        if (!triggered) {
            return result;
        }

        // Find dominant band (band with max positive change from previous frame)
        int dom_idx = 0;
        float dom_change = -1.0f;
        for (int i = 0; i < 16; i++) {
            float change = bands[i] - saved_prev[i];
            if (change > dom_change) {
                dom_change = change;
                dom_idx = i;
            }
        }

        // Compute strength: how much value exceeds threshold, normalized
        float excess = value - threshold;
        float strength = (threshold > 0.0f) ? (excess / threshold) : 1.0f;
        strength = std::max(0.1f, std::min(1.0f, strength));

        result.detected = true;
        result.strength = strength;
        result.dominant_band_index = dom_idx;
        result.dominant_band = band_category(dom_idx);
        result.type = (dom_idx < 4) ? TYPE_BEAT : TYPE_ONSET;

        record_onset(timestamp_ms);
        return result;
    }

    /// Set sensitivity 1-100; maps to multiplier 3.0 (low) to 0.5 (high).
    void set_sensitivity(int value) {
        int clamped = std::max(1, std::min(100, value));
        multiplier_ = 3.0f - (static_cast<float>(clamped) / 100.0f) * 2.5f;
    }

    void set_mode(Mode mode) { mode_ = mode; }

    Mode mode() const { return mode_; }

    float last_onset_value() const { return last_value_; }

    void reset() {
        flux_count_ = 0;
        flux_head_ = 0;
        flux_sum_ = 0.0f;
        flux_sq_sum_ = 0.0f;
        std::memset(flux_ring_, 0, sizeof(flux_ring_));
        interval_count_ = 0;
        interval_head_ = 0;
        std::memset(interval_ring_, 0, sizeof(interval_ring_));
        last_onset_ms_ = 0;
        hysteresis_armed_ = true;
        for (int i = 0; i < 16; i++) prev_bands_[i] = 0.0f;
    }

    /// Estimated BPM from recent onset intervals.
    /// Returns 0 if insufficient data or last onset is stale.
    float current_bpm(uint32_t now_ms) const {
        if (interval_count_ < 3) return 0.0f;
        if (last_onset_ms_ > 0 && (now_ms - last_onset_ms_) > BPM_STALE_MS) {
            return 0.0f;
        }
        // Copy to stack for sorting (small fixed-size array)
        uint32_t sorted[MAX_INTERVALS];
        std::memcpy(sorted, interval_ring_, interval_count_ * sizeof(uint32_t));
        std::sort(sorted, sorted + interval_count_);
        uint32_t median = sorted[interval_count_ / 2];
        if (median == 0) return 0.0f;
        return 60000.0f / static_cast<float>(median);
    }

    /// Confidence 0-100 based on coefficient of variation of intervals.
    int confidence() const {
        if (interval_count_ < 3) return 0;
        float sum = 0.0f;
        for (size_t i = 0; i < interval_count_; i++) sum += static_cast<float>(interval_ring_[i]);
        float mean = sum / static_cast<float>(interval_count_);
        if (mean <= 0.0f) return 0;
        float sq_sum = 0.0f;
        for (size_t i = 0; i < interval_count_; i++) {
            float d = static_cast<float>(interval_ring_[i]) - mean;
            sq_sum += d * d;
        }
        float std_dev = sqrtf(sq_sum / static_cast<float>(interval_count_));
        float cv = std_dev / mean;
        int conf = static_cast<int>((1.0f - std::min(1.0f, cv)) * 100.0f);
        return conf;
    }

 private:
    Mode mode_;
    size_t window_size_;
    uint32_t min_interval_ms_;
    uint32_t last_onset_ms_;
    float multiplier_;
    float prev_bands_[16];
    bool hysteresis_armed_;
    float last_value_{0.0f};

    // Circular array replacing std::vector<float> flux_history_
    static constexpr size_t MAX_WINDOW = 128;
    float flux_ring_[MAX_WINDOW];
    size_t flux_count_;
    size_t flux_head_;
    // Incremental statistics for O(1) threshold computation
    float flux_sum_;
    float flux_sq_sum_;

    // Circular array replacing std::vector<uint32_t> beat_intervals_
    static constexpr size_t MAX_INTERVALS = 16;
    uint32_t interval_ring_[MAX_INTERVALS];
    size_t interval_count_;
    size_t interval_head_;

    static constexpr uint32_t BPM_STALE_MS = 5000;

    float compute_spectral_flux(const float bands[16]) const {
        float flux = 0.0f;
        for (int i = 0; i < 16; i++) {
            float diff = bands[i] - prev_bands_[i];
            if (diff > 0) flux += diff * diff;  // Half-wave rectification
        }
        return flux;
    }

    float compute_threshold() const {
        // Require at least half the window before trusting the threshold
        if (flux_count_ < window_size_ / 2) return 1e10f;
        float n = static_cast<float>(flux_count_);
        float mean = flux_sum_ / n;
        float variance = flux_sq_sum_ / n - mean * mean;
        float std_dev = sqrtf(std::max(0.0f, variance));
        std_dev = std::max(std_dev, mean * 0.1f);  // floor to avoid near-zero std_dev
        return mean + multiplier_ * std_dev;
    }

    void record_onset(uint32_t timestamp_ms) {
        if (last_onset_ms_ > 0) {
            uint32_t interval = timestamp_ms - last_onset_ms_;

            // Outlier rejection: keep if within 0.5x-2x current median
            if (interval_count_ >= 3) {
                uint32_t sorted[MAX_INTERVALS];
                std::memcpy(sorted, interval_ring_, interval_count_ * sizeof(uint32_t));
                std::sort(sorted, sorted + interval_count_);
                uint32_t median = sorted[interval_count_ / 2];
                if (interval < median / 2 || interval > median * 2) {
                    last_onset_ms_ = timestamp_ms;
                    return;
                }
            }

            // Write to circular buffer
            if (interval_count_ < MAX_INTERVALS) {
                interval_ring_[interval_count_] = interval;
                interval_count_++;
            } else {
                interval_ring_[interval_head_] = interval;
                interval_head_ = (interval_head_ + 1) % MAX_INTERVALS;
            }
        }
        last_onset_ms_ = timestamp_ms;
    }

    static const char *band_category(int band_index) {
        if (band_index < 4) return "bass";
        if (band_index < 10) return "mid";
        return "high";
    }
};

}  // namespace audio_reactive
}  // namespace esphome
