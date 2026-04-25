#pragma once

#include <algorithm>
#include <cmath>

namespace esphome {
namespace audio_reactive {

struct AGCPreset {
    float kp;             // Proportional gain
    float ki;             // Integral gain
    float attack_rate;    // Fast follow rate (signal rising)
    float release_rate;   // Slow follow rate (signal falling)
    float target;         // Target output level (0-1 normalized)
    float decay;          // Sample tracking decay factor
    float gain_step;      // Per-frame gain-adjustment scale factor.
    float gain_min;       // Lower clamp on gain. Determines max attenuation:
                          // for raw input X to land at target T, gain must
                          // reach T/X. If T/X < gain_min the AGC saturates.
                          // Basic-tier presets use 1/64 (input pre-scaled).
                          // Pro-tier per-band AGCs need much smaller because
                          // raw mel sums can be in the thousands.
    float gain_max;       // Upper clamp on gain (max amplification).
    float max_adjustment; // Per-frame absolute cap on the PI adjustment
                          // before multiplying by gain_step. Prevents the
                          // gain from crashing through the clamp in one
                          // frame when error is huge (raw inputs in the
                          // thousands, kp=1, |adjustment| ~= |error|).
                          // Basic-tier presets use a high value (effectively
                          // uncapped — historical behavior). Pro-tier AGC_FAST
                          // uses 2.0 so gain_change per frame is bounded to
                          // 2 * gain_step ≈ 0.01, giving smooth ~1-second
                          // convergence regardless of input magnitude.
};

// Basic-tier presets — tuned for inputs already pre-scaled by raw_scale_
// (typically ~1/20), so input dynamic range is roughly 0–1. gain_min/max
// and max_adjustment values match the original hardcoded behaviour.
static constexpr AGCPreset AGC_NORMAL = {0.6f,  1.7f,  1.0f / 192,  1.0f / 6144, 0.5f,  0.9994f, 0.001f, 1.0f / 64.0f,    32.0f, 1000.0f};
static constexpr AGCPreset AGC_VIVID  = {1.5f,  1.85f, 1.0f / 128,  1.0f / 4096, 0.55f, 0.9985f, 0.001f, 1.0f / 64.0f,    32.0f, 1000.0f};
static constexpr AGCPreset AGC_LAZY   = {0.65f, 1.2f,  1.0f / 256,  1.0f / 8192, 0.45f, 0.9997f, 0.001f, 1.0f / 64.0f,    32.0f, 1000.0f};

// Pro-tier per-musical-band preset — used by MusicalBands for RAW mel sums.
// Differences from the basic-tier presets explained inline:
//   release_rate 1/512  ≈ 6 s sample_avg time constant (was 71 s on _NORMAL).
//   attack_rate 1/64    — quicker peak tracking for dynamic music transients.
//   gain_step 0.005     — 5x faster gain wind-up/down per frame than basic.
//   kp / ki bumped      — keeps PI responsive at the larger step size.
//   gain_min 1/16384    — raw mel sums on hardware reach 2000+ during music;
//                         to bring those to target=0.5 requires gain ≈ 1/4000.
//                         The basic-tier 1/64 floor saturated us at ~30x too
//                         high (output stuck at 1.0 even though gain was at
//                         its floor). 1/16384 gives headroom for inputs up
//                         to ~8000 to land at target.
//   max_adjustment 2.0  — caps |adjustment| so a huge raw input (error in
//                         the thousands) doesn't crash gain through the floor
//                         in a single frame. With max=2 and gain_step=0.005,
//                         |gain_change| ≤ 0.01 per frame, giving a smooth
//                         ~1.2-second convergence from gain=1 down to the
//                         floor under sustained loud input.
//   target 0.5          — same as _NORMAL.
//   decay 0.998         — sample_max decays slightly faster, matches faster attack.
static constexpr AGCPreset AGC_FAST   = {1.0f,  2.0f,  1.0f / 64,   1.0f / 512,  0.5f,  0.998f,  0.005f, 1.0f / 16384.0f, 32.0f,    2.0f};

class AGC {
 public:
    explicit AGC(AGCPreset preset = AGC_NORMAL)
        : preset_(preset), gain_(1.0f), integrator_(0.0f),
          sample_avg_(0.0f), sample_max_(0.0f) {}

    /**
     * Process a raw value through the AGC.
     * Returns gain-adjusted value normalized roughly to 0-1 range.
     * Values below the noise floor are suppressed to zero to prevent
     * the AGC from amplifying mic self-noise in quiet environments.
     */
    float process(float raw_value) {
        // Suppress values below noise floor — prevents AGC from amplifying
        // mic self-noise to fill the 0-1 range in quiet rooms.
        // Noise floor is set low enough to not affect real audio signals.
        if (raw_value < noise_floor_) {
            // Don't update tracking — just decay toward zero
            sample_max_ *= preset_.decay;
            return 0.0f;
        }

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
        // Clamp adjustment so a single huge-error frame can't crash gain
        // through the clamp in one step. See AGC_FAST docs.
        adjustment = std::max(-preset_.max_adjustment,
                              std::min(preset_.max_adjustment, adjustment));
        gain_ += adjustment * preset_.gain_step;

        // Clamp gain to the preset's allowed range.
        gain_ = std::max(preset_.gain_min, std::min(preset_.gain_max, gain_));

        // Apply gain and clamp output
        float result = raw_value * gain_;
        return std::max(0.0f, std::min(1.0f, result));
    }

    /**
     * Set the noise floor threshold. Raw values below this are suppressed to zero.
     * Default 0.5 — typical PDM mic self-noise RMS from FFT.
     */
    void set_noise_floor(float floor) { noise_floor_ = floor; }

    /**
     * Switch the active preset and reset the gain-tracking state. Used to apply
     * a tier-specific preset (e.g. AGC_FAST for the per-musical-band AGCs in
     * MusicalBands) after the AGC has been default-constructed.
     */
    void set_preset(AGCPreset preset) {
        preset_ = preset;
        // Reset PI state so the new time constants are not applied on top of
        // wound-up integrator/gain values from the previous preset.
        gain_ = 1.0f;
        integrator_ = 0.0f;
        sample_avg_ = 0.0f;
        sample_max_ = 0.0f;
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
    float noise_floor_{0.0f};  // Raw RMS values below this are suppressed. Set per-band by main component.
};

}  // namespace audio_reactive
}  // namespace esphome
