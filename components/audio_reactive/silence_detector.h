#pragma once

#include <algorithm>
#include <cstdint>

namespace esphome {
namespace audio_reactive {

/// Detects silence by comparing raw amplitude against an adjustable squelch gate.
class SilenceDetector {
 public:
    explicit SilenceDetector(float default_squelch = 10.0f)
        : squelch_(default_squelch) {}

    struct Result {
        bool is_below_gate;  // Raw amplitude is below the squelch threshold
        bool is_silent;      // Below gate for more than 1 second
    };

    /// Set a direct threshold value (bypasses squelch→threshold mapping).
    /// Used by calibration to set threshold from measured quiet room levels.
    void set_squelch_threshold_direct(float threshold) {
        direct_threshold_ = threshold;
        use_direct_threshold_ = true;
    }

    /// Process a new amplitude sample.
    /// @param raw_amplitude  Raw RMS amplitude from FFT (not normalized — scale varies by mic/gain)
    /// @param timestamp_ms   Monotonic timestamp in milliseconds
    Result update(float raw_amplitude, uint32_t timestamp_ms) {
        // Map squelch 0-100 to threshold.
        // Input signal is mid+high energy (not raw amplitude).
        // Quiet room mid+high ≈ 1-4, music ≈ 10-25.
        // At default squelch=10, threshold=5 gates quiet room noise.
        // Scale: squelch=10 → 5, squelch=50 → 25, squelch=100 → 50.
        float user_threshold = squelch_ * 0.5f;

        // If calibration set a direct threshold, use the LOWER of the two.
        // This lets the user's squelch slider always override a high calibration
        // threshold (e.g. ES8311 codec noise produces squelch_thresh ≈ 28, but
        // user can set squelch=3 → threshold=1.5 to let music through).
        float threshold = use_direct_threshold_
            ? std::min(direct_threshold_, user_threshold)
            : user_threshold;

        bool below = raw_amplitude < threshold;

        if (below) {
            if (!below_gate_) {
                below_gate_ = true;
                gate_start_ms_ = timestamp_ms;
            }
            // Transition to silent after 1 second below gate
            bool now_silent = (timestamp_ms - gate_start_ms_) >= 1000;
            silent_ = now_silent;
        } else {
            below_gate_ = false;
            silent_ = false;
        }

        return {below_gate_, silent_};
    }

    /// Set squelch level 0-100. Higher = louder signal required to pass gate.
    void set_squelch(float value) {
        squelch_ = std::max(0.0f, std::min(100.0f, value));
    }

    float squelch() const { return squelch_; }

    /// Return the effective threshold used by the silence gate.
    float effective_threshold() const {
        float user_threshold = squelch_ * 0.5f;
        return use_direct_threshold_
            ? std::min(direct_threshold_, user_threshold)
            : user_threshold;
    }

 private:
    float squelch_;
    float direct_threshold_{0.0f};
    bool use_direct_threshold_{false};
    uint32_t gate_start_ms_{0};
    bool below_gate_{false};
    bool silent_{false};
};

}  // namespace audio_reactive
}  // namespace esphome
