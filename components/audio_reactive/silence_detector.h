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

    /// Process a new amplitude sample.
    /// @param raw_amplitude  Raw RMS amplitude from FFT (not normalized — scale varies by mic/gain)
    /// @param timestamp_ms   Monotonic timestamp in milliseconds
    Result update(float raw_amplitude, uint32_t timestamp_ms) {
        // Map squelch 0-100 to threshold.
        // Raw FFT magnitudes from a PDM mic typically produce RMS values in the
        // range 0-50+ for loud signals, with noise floor around 0.5-5.0 depending
        // on hardware. Scale squelch linearly: squelch=10 → threshold=1.0,
        // squelch=50 → threshold=5.0, squelch=100 → threshold=10.0.
        float threshold = squelch_ / 10.0f;

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

 private:
    float squelch_;
    uint32_t gate_start_ms_{0};
    bool below_gate_{false};
    bool silent_{false};
};

}  // namespace audio_reactive
}  // namespace esphome
