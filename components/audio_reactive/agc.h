#pragma once

#include <algorithm>
#include <cmath>

namespace esphome {
namespace audio_reactive {

struct AGCPreset {
    float kp;            // Proportional gain
    float ki;            // Integral gain
    float attack_rate;   // Fast follow rate (signal rising)
    float release_rate;  // Slow follow rate (signal falling)
    float target;        // Target output level (0-1 normalized)
    float decay;         // Sample tracking decay factor
};

static constexpr AGCPreset AGC_NORMAL = {0.6f,  1.7f,  1.0f / 192,  1.0f / 6144, 0.5f,  0.9994f};
static constexpr AGCPreset AGC_VIVID  = {1.5f,  1.85f, 1.0f / 128,  1.0f / 4096, 0.55f, 0.9985f};
static constexpr AGCPreset AGC_LAZY   = {0.65f, 1.2f,  1.0f / 256,  1.0f / 8192, 0.45f, 0.9997f};

class AGC {
 public:
    explicit AGC(AGCPreset preset = AGC_NORMAL)
        : preset_(preset), gain_(1.0f), integrator_(0.0f),
          sample_avg_(0.0f), sample_max_(0.0f) {}

    /**
     * Process a raw value through the AGC.
     * Returns gain-adjusted value normalized roughly to 0-1 range.
     */
    float process(float raw_value) {
        // Track signal level
        if (raw_value > sample_max_) {
            sample_max_ = sample_max_ + preset_.attack_rate * (raw_value - sample_max_);
        } else {
            sample_max_ = sample_max_ * preset_.decay;
        }
        sample_avg_ = sample_avg_ + preset_.release_rate * (raw_value - sample_avg_);

        // PI controller
        float error = preset_.target - (sample_avg_ * gain_);
        integrator_ += error * preset_.ki * 0.001f;
        integrator_ = std::max(-2.0f, std::min(2.0f, integrator_));

        float adjustment = error * preset_.kp + integrator_;
        gain_ += adjustment * 0.001f;

        // Clamp gain to reasonable range
        gain_ = std::max(1.0f / 64.0f, std::min(32.0f, gain_));

        // Apply gain and clamp output
        float result = raw_value * gain_;
        return std::max(0.0f, std::min(1.0f, result));
    }

    /**
     * Suspend gain adjustments (call during silence).
     * Slowly decays the integrator.
     */
    void suspend() {
        integrator_ *= 0.91f;
    }

    void reset() {
        gain_ = 1.0f;
        integrator_ = 0.0f;
        sample_avg_ = 0.0f;
        sample_max_ = 0.0f;
    }

    float current_gain() const { return gain_; }

 private:
    AGCPreset preset_;
    float gain_;
    float integrator_;
    float sample_avg_;
    float sample_max_;
};

}  // namespace audio_reactive
}  // namespace esphome
