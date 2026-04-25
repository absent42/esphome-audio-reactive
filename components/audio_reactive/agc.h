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
    float gain_step;     // Per-frame gain-adjustment scale factor.
                         // Larger = faster gain convergence; smaller = more
                         // stable. The classic basic-tier presets used a
                         // hardcoded 0.001f multiplier — kept here as default
                         // for those. Pro-tier per-band AGCs (AGC_FAST) use
                         // a larger value so gain catches up to raw mel-band
                         // dynamics in seconds rather than minutes.
};

// Basic-tier presets — tuned for inputs already pre-scaled by raw_scale_
// (typically ~1/20), so input dynamic range is roughly 0–1.
static constexpr AGCPreset AGC_NORMAL = {0.6f,  1.7f,  1.0f / 192,  1.0f / 6144, 0.5f,  0.9994f, 0.001f};
static constexpr AGCPreset AGC_VIVID  = {1.5f,  1.85f, 1.0f / 128,  1.0f / 4096, 0.55f, 0.9985f, 0.001f};
static constexpr AGCPreset AGC_LAZY   = {0.65f, 1.2f,  1.0f / 256,  1.0f / 8192, 0.45f, 0.9997f, 0.001f};

// Pro-tier per-musical-band preset — used by MusicalBands for raw mel sums.
// Mel sums are NOT pre-scaled by raw_scale_, so inputs span a much wider and
// less predictable range than basic-tier inputs. Convergence has to be fast
// enough that the AGC reaches steady state during a typical music passage
// instead of staying in the wind-up transient (which manifests as either
// saturated output or under-amplified silence depending on signal level).
//   release_rate 1/512 ≈ 6 s sample_avg time constant (was 71 s on _NORMAL).
//   attack_rate 1/64 — quicker peak tracking for dynamic music transients.
//   gain_step 0.005 — 5x faster gain wind-up/down per frame than basic.
//   kp / ki bumped to keep the PI loop responsive at the new step size.
//   target 0.5 — same as _NORMAL.
//   decay 0.998 — sample_max decays slightly faster, matches faster attack.
static constexpr AGCPreset AGC_FAST   = {1.0f,  2.0f,  1.0f / 64,   1.0f / 512,  0.5f,  0.998f,  0.005f};

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
        gain_ += adjustment * preset_.gain_step;

        // Clamp gain to reasonable range
        gain_ = std::max(1.0f / 64.0f, std::min(32.0f, gain_));

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
