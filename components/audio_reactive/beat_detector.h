#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace audio_reactive {

/// Bass-energy threshold beat detector with BPM tracking.
class BeatDetector {
 public:
    /// @param sensitivity      1-100 (higher = lower threshold = more beats)
    /// @param window_size      Rolling average window in samples
    /// @param min_interval_ms  Minimum ms between beats (prevents flicker)
    BeatDetector(int sensitivity = 50, size_t window_size = 20,
                 uint32_t min_interval_ms = 150)
        : window_size_(window_size),
          min_interval_ms_(min_interval_ms),
          last_beat_ms_(0) {
        // Map sensitivity 1-100 to multiplier 3.0-0.5
        int clamped = std::max(1, std::min(100, sensitivity));
        multiplier_ = 3.0f - (static_cast<float>(clamped) / 100.0f) * 2.5f;
        samples_.reserve(window_size);
        beat_intervals_.reserve(8);
    }

    /// Process a new bass energy sample.
    /// @param bass_energy  Normalized bass energy (0.0-1.0)
    /// @param timestamp_ms  Monotonic timestamp in milliseconds
    /// @returns true if a beat was detected
    bool update(float bass_energy, uint32_t timestamp_ms) {
        // Add to rolling window
        if (samples_.size() >= window_size_) {
            samples_.erase(samples_.begin());
        }
        samples_.push_back(bass_energy);

        // Need enough samples for baseline
        if (samples_.size() < window_size_ / 2) {
            return false;
        }

        float threshold = compute_threshold();
        threshold_ = threshold;

        if (bass_energy <= threshold) {
            return false;
        }

        // Enforce minimum interval
        if (last_beat_ms_ > 0 &&
            (timestamp_ms - last_beat_ms_) < min_interval_ms_) {
            return false;
        }

        // Record beat for BPM tracking
        if (last_beat_ms_ > 0) {
            uint32_t interval = timestamp_ms - last_beat_ms_;
            if (beat_intervals_.size() >= 8) {
                beat_intervals_.erase(beat_intervals_.begin());
            }
            beat_intervals_.push_back(interval);
        }
        last_beat_ms_ = timestamp_ms;
        return true;
    }

    /// Current dynamic threshold (for diagnostics).
    float current_threshold() const { return threshold_; }

    /// Estimated BPM from recent beat intervals. Returns 0 if insufficient data.
    float current_bpm() const {
        if (beat_intervals_.size() < 3) return 0.0f;
        // Use median interval for robustness against outliers
        std::vector<uint32_t> sorted = beat_intervals_;
        std::sort(sorted.begin(), sorted.end());
        uint32_t median = sorted[sorted.size() / 2];
        if (median == 0) return 0.0f;
        return 60000.0f / static_cast<float>(median);
    }

 private:
    size_t window_size_;
    uint32_t min_interval_ms_;
    uint32_t last_beat_ms_;
    float multiplier_;
    float threshold_ = 0.0f;
    std::vector<float> samples_;
    std::vector<uint32_t> beat_intervals_;

    float compute_threshold() const {
        if (samples_.empty()) return 1.0f;
        float sum = 0.0f;
        for (float s : samples_) sum += s;
        float avg = sum / static_cast<float>(samples_.size());

        float var_sum = 0.0f;
        for (float s : samples_) {
            float diff = s - avg;
            var_sum += diff * diff;
        }
        float std_dev = sqrtf(var_sum / static_cast<float>(samples_.size()));
        // Use avg as floor so threshold still works with near-zero std_dev
        std_dev = std::max(std_dev, avg * 0.1f);

        return avg + multiplier_ * std_dev;
    }
};

}  // namespace audio_reactive
