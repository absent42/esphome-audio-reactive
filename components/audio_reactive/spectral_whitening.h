#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace esphome {
namespace audio_reactive {

/// Adaptive spectral whitening: normalizes each FFT bin by a running peak
/// estimate with exponential decay.
///
/// Each bin's peak tracks the maximum magnitude seen, then decays at rate
/// `decay_` per frame. This flattens the long-term spectral shape so that
/// onset detection responds to transients rather than sustained tones.
///
/// Apply to a COPY of the magnitude buffer; the original is preserved for
/// band-energy sensors that should reflect the true spectral shape.
///
/// @tparam MAX_BINS  Maximum number of FFT bins (e.g. 256 for 512-point FFT)
template <size_t MAX_BINS>
class SpectralWhitening {
 public:
    /// @param update_rate_hz  Frame rate at which process() is called (e.g. 50 Hz)
    /// @param relax_time_s    Peak decay time constant in seconds (default 2s)
    /// @param floor           Minimum denominator to prevent division by zero
    explicit SpectralWhitening(float update_rate_hz,
                                float relax_time_s = 2.0f,
                                float floor = 1e-4f)
        : floor_(floor) {
        set_relax_time(update_rate_hz, relax_time_s);
        reset();
    }

    /// Whiten magnitude spectrum in-place.
    /// DC bin (index 0) is left unchanged.
    ///
    /// The key property: each bin is normalized by the *previous* (decayed)
    /// peak before the peak is updated with the current magnitude. This means
    /// a sudden transient still appears large (it is divided by the old, lower
    /// peak) while sustained tones eventually converge to ~1.0.
    ///
    /// @param magnitudes  Array of FFT bin magnitudes, modified in-place.
    /// @param bin_count   Number of bins to process (must be <= MAX_BINS).
    void process(float *magnitudes, size_t bin_count) {
        size_t count = std::min(bin_count, MAX_BINS);
        // Skip DC bin (index 0)
        for (size_t i = 1; i < count; i++) {
            float orig = magnitudes[i];
            // Decay the existing peak estimate
            float decayed = peaks_[i] * decay_;
            // Normalize by the decayed (old) peak — before updating with current mag.
            // This preserves transients: a sudden spike is divided by the previous
            // low peak, so it still appears prominent in the output.
            magnitudes[i] /= std::max(decayed, floor_);
            // Update running peak: track maximum of original (pre-normalized) mag
            // and the decayed estimate.
            peaks_[i] = std::max(orig, decayed);
        }
    }

    /// Reset all peak estimates to the floor value.
    void reset() {
        for (size_t i = 0; i < MAX_BINS; i++) {
            peaks_[i] = floor_;
        }
    }

    /// Update the decay coefficient from a new rate / relax-time pair.
    void set_relax_time(float update_rate_hz, float relax_time_s) {
        // decay_ = exp(-1 / (update_rate * relax_time))
        // At decay_ close to 1 peaks decay slowly (long memory).
        decay_ = std::exp(-1.0f / (update_rate_hz * relax_time_s));
    }

 private:
    float peaks_[MAX_BINS];
    float decay_;
    float floor_;
};

}  // namespace audio_reactive
}  // namespace esphome
