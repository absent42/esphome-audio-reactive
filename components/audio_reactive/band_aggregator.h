#pragma once

#include <cmath>
#include <cstddef>

namespace esphome {
namespace audio_reactive {

/// Frequency band boundaries in Hz.
constexpr float BASS_LOW_HZ = 20.0f;
constexpr float BASS_HIGH_HZ = 350.0f;
constexpr float MID_LOW_HZ = 350.0f;
constexpr float MID_HIGH_HZ = 2000.0f;
constexpr float HIGH_LOW_HZ = 2000.0f;
constexpr float HIGH_HIGH_HZ = 5000.0f;

struct BandBinRanges {
    size_t bass_start, bass_end;
    size_t mid_start, mid_end;
    size_t high_start, high_end;
};

struct BandEnergies {
    float bass;       // Raw RMS bass energy (normalized to 0-1 by AGC later)
    float mid;        // Raw RMS mid energy
    float high;       // Raw RMS high energy
    float amplitude;  // Raw RMS overall amplitude
};

/// Groups FFT magnitude bins into bass/mid/high energy bands.
class BandAggregator {
 public:
    /// @param freq_resolution  Hz per FFT bin (sample_rate / fft_size).
    explicit BandAggregator(float freq_resolution)
        : freq_res_(freq_resolution) {}

    /// Compute bin index ranges for each frequency band.
    BandBinRanges band_bin_ranges(size_t bin_count) const {
        BandBinRanges r{};
        r.bass_start = freq_to_bin(BASS_LOW_HZ, bin_count);
        r.bass_end = freq_to_bin(BASS_HIGH_HZ, bin_count);
        r.mid_start = r.bass_end + 1;
        r.mid_end = freq_to_bin(MID_HIGH_HZ, bin_count);
        r.high_start = r.mid_end + 1;
        r.high_end = freq_to_bin(HIGH_HIGH_HZ, bin_count);
        return r;
    }

    /// Aggregate FFT magnitudes into band energies.
    BandEnergies aggregate(const float* magnitudes, size_t bin_count) const {
        auto ranges = band_bin_ranges(bin_count);
        BandEnergies result{};
        result.bass = rms_energy(magnitudes, ranges.bass_start, ranges.bass_end);
        result.mid = rms_energy(magnitudes, ranges.mid_start, ranges.mid_end);
        result.high = rms_energy(magnitudes, ranges.high_start, ranges.high_end);
        result.amplitude = rms_energy(magnitudes, 1, bin_count - 1);
        return result;
    }

 private:
    float freq_res_;

    size_t freq_to_bin(float freq, size_t bin_count) const {
        size_t bin = static_cast<size_t>(freq / freq_res_);
        if (bin == 0) bin = 1;
        return (bin < bin_count) ? bin : bin_count - 1;
    }

    static float rms_energy(const float* data, size_t start, size_t end) {
        if (start > end) return 0.0f;
        float sum = 0.0f;
        for (size_t i = start; i <= end; i++) {
            sum += data[i] * data[i];
        }
        return sqrtf(sum / static_cast<float>(end - start + 1));
    }
};

}  // namespace audio_reactive
}  // namespace esphome
