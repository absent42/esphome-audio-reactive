#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome {
namespace audio_reactive {

/// Onset detector supporting spectral flux, bass-energy, and complex-domain modes.
/// BPM tracking is handled by BeatTracker (separate class).
class OnsetDetector {
 public:
    enum Mode { MODE_SPECTRAL_FLUX, MODE_BASS_ENERGY, MODE_COMPLEX_DOMAIN };

    struct OnsetResult {
        bool detected;
        float strength;  // 0.1-1.0, floored at 0.1
    };

    /// @param sensitivity      1-100 (higher = lower threshold = more detections)
    /// @param mode             Spectral flux, bass energy, or complex domain
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
          flux_sq_sum_(0.0f) {
        set_sensitivity(sensitivity);
        for (int i = 0; i < 16; i++) prev_bands_[i] = 0.0f;
        std::memset(flux_ring_, 0, sizeof(flux_ring_));
    }

    /// Process a new frame of band energies.
    OnsetResult update(const float bands[16], float bass_energy, uint32_t timestamp_ms,
                       float external_onset_value = -1.0f) {
        OnsetResult result{false, 0.0f};

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

        // Compute strength: how much value exceeds threshold, normalized
        float excess = value - threshold;
        float strength = (threshold > 0.0f) ? (excess / threshold) : 1.0f;
        strength = std::max(0.1f, std::min(1.0f, strength));

        result.detected = true;
        result.strength = strength;

        last_onset_ms_ = timestamp_ms;
        return result;
    }

    /// Set sensitivity 1-100; maps to multiplier 3.0 (low) to 0.5 (high).
    void set_sensitivity(int value) {
        int clamped = std::max(1, std::min(100, value));
        multiplier_ = 3.0f - (static_cast<float>(clamped) / 100.0f) * 2.5f;
    }

    void set_mode(Mode mode) { mode_ = mode; }

    float last_onset_value() const { return last_value_; }

    void reset() {
        flux_count_ = 0;
        flux_head_ = 0;
        flux_sum_ = 0.0f;
        flux_sq_sum_ = 0.0f;
        std::memset(flux_ring_, 0, sizeof(flux_ring_));
        last_onset_ms_ = 0;
        hysteresis_armed_ = true;
        for (int i = 0; i < 16; i++) prev_bands_[i] = 0.0f;
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

    // Circular array for rolling onset values
    static constexpr size_t MAX_WINDOW = 128;
    float flux_ring_[MAX_WINDOW];
    size_t flux_count_;
    size_t flux_head_;
    float flux_sum_;
    float flux_sq_sum_;

    float compute_spectral_flux(const float bands[16]) const {
        float flux = 0.0f;
        for (int i = 0; i < 16; i++) {
            float diff = bands[i] - prev_bands_[i];
            if (diff > 0) flux += diff * diff;
        }
        return flux;
    }

    float compute_threshold() const {
        if (flux_count_ < window_size_ / 2) return 1e10f;
        float n = static_cast<float>(flux_count_);
        float mean = flux_sum_ / n;
        float variance = flux_sq_sum_ / n - mean * mean;
        float std_dev = sqrtf(std::max(0.0f, variance));
        std_dev = std::max(std_dev, mean * 0.1f);
        return mean + multiplier_ * std_dev;
    }
};

}  // namespace audio_reactive
}  // namespace esphome
