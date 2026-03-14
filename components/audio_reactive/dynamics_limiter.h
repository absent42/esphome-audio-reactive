#pragma once

#include <algorithm>
#include <cmath>

namespace esphome {
namespace audio_reactive {

/// Rate-limits signal rise (attack) and fall (decay) to smooth abrupt changes.
///
/// The rate constants are expressed as ms to traverse 196 units, matching the
/// scale used by the AGC output (0-196 raw range before normalization).
class DynamicsLimiter {
 public:
    /// @param attack_ms  Time (ms) to rise 196 units (fast follow on signal increase)
    /// @param decay_ms   Time (ms) to fall 196 units (slow follow on signal decrease)
    DynamicsLimiter(float attack_ms = 80.0f, float decay_ms = 1400.0f)
        : attack_ms_(attack_ms), decay_ms_(decay_ms), last_value_(0.0f) {}

    /// Process a new value with rate limiting.
    /// @param value     Target value
    /// @param delta_ms  Time elapsed since last call in milliseconds
    float process(float value, float delta_ms) {
        if (delta_ms <= 0.0f) return last_value_;

        float max_rise = 196.0f * delta_ms / attack_ms_;
        float max_fall = 196.0f * delta_ms / decay_ms_;

        if (value > last_value_) {
            // Attack: limit rise rate
            last_value_ = std::min(value, last_value_ + max_rise);
        } else {
            // Decay: limit fall rate
            last_value_ = std::max(value, last_value_ - max_fall);
        }

        return last_value_;
    }

    void reset() { last_value_ = 0.0f; }

    float last_value() const { return last_value_; }

 private:
    float attack_ms_;
    float decay_ms_;
    float last_value_;
};

}  // namespace audio_reactive
}  // namespace esphome
