#pragma once

namespace esphome {
namespace audio_reactive {

/// Weighted mean frequency of the spectrum (spectral centroid).
///
/// Computes sum(bin * mag[bin]) / sum(mag[bin]) over bins 1..bin_count-1,
/// then multiplies by hz_per_bin. DC bin (index 0) is skipped.
///
/// Returns 0 if the spectrum is silent (all magnitudes zero).
inline float spectral_centroid(const float *magnitudes, int bin_count, float hz_per_bin) {
    float weighted_sum = 0.0f;
    float total_mag    = 0.0f;

    for (int i = 1; i < bin_count; i++) {
        weighted_sum += static_cast<float>(i) * magnitudes[i];
        total_mag    += magnitudes[i];
    }

    if (total_mag == 0.0f)
        return 0.0f;

    return (weighted_sum / total_mag) * hz_per_bin;
}

/// Frequency below which `fraction` of total spectral energy is concentrated.
///
/// Energy is defined as mag^2 per bin. Iterates bins 1..bin_count-1 (DC skipped),
/// accumulating energy until the running sum reaches `fraction` of the total.
/// Returns the frequency of that bin in Hz.
///
/// Returns 0 if the spectrum is silent (all magnitudes zero).
inline float spectral_rolloff(const float *magnitudes, int bin_count, float hz_per_bin,
                               float fraction = 0.85f) {
    float total_energy = 0.0f;
    for (int i = 1; i < bin_count; i++) {
        total_energy += magnitudes[i] * magnitudes[i];
    }

    if (total_energy == 0.0f)
        return 0.0f;

    float threshold    = fraction * total_energy;
    float running_sum  = 0.0f;

    for (int i = 1; i < bin_count; i++) {
        running_sum += magnitudes[i] * magnitudes[i];
        if (running_sum >= threshold)
            return static_cast<float>(i) * hz_per_bin;
    }

    // Fallback: return frequency of last bin (shouldn't normally be reached).
    return static_cast<float>(bin_count - 1) * hz_per_bin;
}

}  // namespace audio_reactive
}  // namespace esphome
