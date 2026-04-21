#pragma once

// Log-mel filterbank for pro-tier DSP.
// Computes mel-frequency energies from FFT magnitudes using triangular filters.
// Sparse weight storage: flat weight array + per-band [start_bin, end_bin) offsets.
// Memory footprint: ~6KB for N_MEL=32, N_FFT=2048 (per spec budget).
//
// Output is RAW (pre-log) — consumers that need log-domain values apply logf()
// themselves (SuperFlux does this). MusicalBands aggregates raw values in the
// linear/energy domain (sum of bin energies = total energy in band).

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace esphome {
namespace audio_reactive {

template <uint16_t N_MEL, uint16_t N_FFT>
class MelFilterbank {
 public:
    static constexpr uint16_t kNumBands = N_MEL;
    static constexpr uint16_t kNumBins = N_FFT / 2;
    // Upper bound on total non-zero weights across all bands.
    // Each triangular filter covers (bin_points[m+2] - bin_points[m]) bins.
    // Sum over all bands is bounded by 2 * kNumBins (adjacent triangles share a slope).
    static constexpr uint16_t kMaxTotalWeights = 2 * kNumBins;

    /// Initialize with sample rate and frequency range.
    /// `magnitudes_sq` passed to process() must have length kNumBins.
    void setup(float sample_rate, float freq_min = 40.0f, float freq_max = 16000.0f) {
        sample_rate_ = sample_rate;
        compute_filterbank_(freq_min, freq_max);
    }

    /// Compute raw mel-band energies from a squared-magnitude spectrum (length kNumBins).
    /// Writes N_MEL floats into out_bands (raw/linear, NOT log-compressed).
    /// Consumers that need log-domain values apply logf() themselves (SuperFlux does this).
    /// MusicalBands consumes these raw values directly — sum in linear domain is
    /// semantically correct ("total energy in band = sum of bin energies").
    void process(const float *magnitudes_sq, float *out_bands) const {
        for (uint16_t m = 0; m < N_MEL; m++) {
            float energy = 0.0f;
            const uint16_t start = start_bin_[m];
            const uint16_t end = end_bin_[m];
            const uint16_t offset = weight_offset_[m];
            for (uint16_t b = start; b < end; b++) {
                energy += magnitudes_sq[b] * weights_flat_[offset + (b - start)];
            }
            out_bands[m] = energy;
        }
    }

 protected:
    float sample_rate_{44100.0f};
    // Sparse storage: flat weight array + per-band offsets.
    std::array<uint16_t, N_MEL> start_bin_{};
    std::array<uint16_t, N_MEL> end_bin_{};
    std::array<uint16_t, N_MEL> weight_offset_{};
    std::array<float, kMaxTotalWeights> weights_flat_{};
    uint16_t total_weights_{0};

    /// Convert Hz to mel-scale.
    static float hz_to_mel(float hz) {
        return 2595.0f * log10f(1.0f + hz / 700.0f);
    }
    /// Convert mel-scale to Hz.
    static float mel_to_hz(float mel) {
        return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f);
    }

    /// Build triangular mel filters over the FFT bins, stored sparsely.
    void compute_filterbank_(float freq_min, float freq_max) {
        const float mel_min = hz_to_mel(freq_min);
        const float mel_max = hz_to_mel(freq_max);
        const float bin_hz = sample_rate_ / N_FFT;

        // N_MEL+2 mel-spaced points (bracketing endpoints for triangle bases).
        std::array<float, N_MEL + 2> mel_points{};
        for (uint16_t i = 0; i < N_MEL + 2; i++) {
            mel_points[i] = mel_min + (mel_max - mel_min) * i / (N_MEL + 1);
        }

        std::array<uint16_t, N_MEL + 2> bin_points{};
        for (uint16_t i = 0; i < N_MEL + 2; i++) {
            float hz = mel_to_hz(mel_points[i]);
            uint16_t bin = static_cast<uint16_t>(hz / bin_hz);
            if (bin >= kNumBins) bin = kNumBins - 1;
            bin_points[i] = bin;
        }

        total_weights_ = 0;
        for (uint16_t m = 0; m < N_MEL; m++) {
            const uint16_t left = bin_points[m];
            const uint16_t center = bin_points[m + 1];
            const uint16_t right = bin_points[m + 2];
            start_bin_[m] = left;
            end_bin_[m] = right;
            weight_offset_[m] = total_weights_;

            const uint16_t width = right - left;
            if (total_weights_ + width > kMaxTotalWeights) {
                // Defensive: fall back to empty band rather than overflowing.
                end_bin_[m] = left;
                continue;
            }

            // Build triangle, compute normalization sum, then emit normalized weights.
            float sum = 0.0f;
            for (uint16_t b = left; b < right; b++) {
                float w;
                if (b < center) {
                    w = (center > left) ? static_cast<float>(b - left) / (center - left) : 0.0f;
                } else {
                    w = (right > center) ? static_cast<float>(right - b) / (right - center) : 0.0f;
                }
                weights_flat_[total_weights_ + (b - left)] = w;
                sum += w;
            }
            if (sum > 0.0f) {
                for (uint16_t i = 0; i < width; i++) {
                    weights_flat_[total_weights_ + i] /= sum;
                }
            }
            total_weights_ += width;
        }
    }
};

}  // namespace audio_reactive
}  // namespace esphome
