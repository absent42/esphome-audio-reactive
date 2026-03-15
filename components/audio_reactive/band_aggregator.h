#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace audio_reactive {

/// Summary band energies published to Home Assistant.
struct BandEnergies {
    float bass;       // Raw RMS bass energy (normalized to 0-1 by AGC later)
    float mid;        // Raw RMS mid energy
    float high;       // Raw RMS high energy
    float amplitude;  // Raw RMS overall amplitude
};

/// Full 16-band internal representation.
struct BandEnergies16 {
    float bands[16];  // Per-band RMS energy
    float bass;       // Summary: RMS of bands 0-3
    float mid;        // Summary: RMS of bands 4-9
    float high;       // Summary: RMS of bands 10-15
    float amplitude;  // Overall RMS (all bins except DC)
};

/// 17 frequency boundaries (in Hz) defining 16 bands.
/// Derived from the original WLED-based bin indices at 22050 Hz / 512-point FFT (43.066 Hz/bin).
/// Band i spans [BAND_FREQ_BOUNDARIES[i], BAND_FREQ_BOUNDARIES[i+1]).
static constexpr float BAND_FREQ_BOUNDARIES[17] = {
    43.06640625f,     // bin 1   — start of band 0
    129.19921875f,    // bin 3   — start of band 1
    215.33203125f,    // bin 5   — start of band 2
    344.53125f,       // bin 8   — start of band 3
    473.73046875f,    // bin 11  — start of band 4
    645.99609375f,    // bin 15  — start of band 5
    861.328125f,      // bin 20  — start of band 6
    1162.79296875f,   // bin 27  — start of band 7
    1550.390625f,     // bin 36  — start of band 8
    2024.12109375f,   // bin 47  — start of band 9
    2627.05078125f,   // bin 61  — start of band 10
    3402.24609375f,   // bin 79  — start of band 11
    4392.7734375f,    // bin 102 — start of band 12
    5641.69921875f,   // bin 131 — start of band 13
    7278.22265625f,   // bin 169 — start of band 14
    9388.4765625f,    // bin 218 — start of band 15
    11025.0f,         // bin 256 — end of band 15 (Nyquist at 22050/512)
};

struct BandDefinition {
    size_t bin_start;
    size_t bin_end;  // exclusive
};

/// Groups FFT magnitude bins into 16 bands plus bass/mid/high summary energies.
class BandAggregator {
 public:
    /// Default constructor: 22050 Hz sample rate, 512-point FFT (backwards compatible).
    BandAggregator() : BandAggregator(22050.0f, 512) {}

    /// Construct with explicit sample rate and FFT size.
    /// Dynamically computes bin-to-band mappings from frequency boundaries.
    BandAggregator(float sample_rate, uint16_t fft_size)
        : hz_per_bin_(sample_rate / static_cast<float>(fft_size)) {
        size_t num_bins = fft_size / 2;  // Nyquist limit
        for (int b = 0; b < 16; b++) {
            size_t bin_start = freq_to_bin(BAND_FREQ_BOUNDARIES[b]);
            size_t bin_end   = freq_to_bin(BAND_FREQ_BOUNDARIES[b + 1]);
            // Clamp to available bins
            if (bin_start >= num_bins) bin_start = num_bins;
            if (bin_end > num_bins)    bin_end = num_bins;
            // Ensure at least 1 bin per band (if start < num_bins)
            if (bin_end <= bin_start && bin_start < num_bins) bin_end = bin_start + 1;
            bands_[b] = {bin_start, bin_end};
        }
    }

    /// Aggregate FFT magnitudes into 16-band energies with summary values.
    /// @param magnitudes  FFT magnitude bins (index 0 = DC bin, skipped).
    /// @param bin_count   Number of bins in the magnitudes array.
    BandEnergies16 aggregate16(const float* magnitudes, size_t bin_count) const {
        BandEnergies16 result{};

        for (int b = 0; b < 16; b++) {
            size_t start = bands_[b].bin_start;
            size_t end = bands_[b].bin_end;  // exclusive
            // Clamp to available bins
            if (start >= bin_count) {
                result.bands[b] = 0.0f;
                continue;
            }
            if (end > bin_count) end = bin_count;
            result.bands[b] = rms_energy_exclusive(magnitudes, start, end);
        }

        // Summary: bass = bands 0-3, mid = bands 4-9, high = bands 10-15
        result.bass = rms_of_bands(result.bands, 0, 4);
        result.mid  = rms_of_bands(result.bands, 4, 10);
        result.high = rms_of_bands(result.bands, 10, 16);

        // Amplitude: RMS of all bins except DC (bin 0)
        result.amplitude = (bin_count > 1)
            ? rms_energy_exclusive(magnitudes, 1, bin_count)
            : 0.0f;

        return result;
    }

    /// Aggregate FFT magnitudes into summary 3-band energies (backward compatible).
    /// Internally calls aggregate16() and extracts the summary values.
    BandEnergies aggregate(const float* magnitudes, size_t bin_count) const {
        auto r16 = aggregate16(magnitudes, bin_count);
        BandEnergies result{};
        result.bass      = r16.bass;
        result.mid       = r16.mid;
        result.high      = r16.high;
        result.amplitude = r16.amplitude;
        return result;
    }

    /// Access computed band definitions (for testing/inspection).
    const BandDefinition& band(int index) const { return bands_[index]; }

    /// Get the computed Hz-per-bin value.
    float hz_per_bin() const { return hz_per_bin_; }

 private:
    float hz_per_bin_;
    BandDefinition bands_[16];

    /// Convert a frequency (Hz) to the nearest FFT bin index.
    size_t freq_to_bin(float freq_hz) const {
        float bin = freq_hz / hz_per_bin_;
        return static_cast<size_t>(roundf(bin));
    }

    /// RMS energy of bins [start, end) — end is exclusive.
    static float rms_energy_exclusive(const float* data, size_t start, size_t end) {
        if (start >= end) return 0.0f;
        float sum = 0.0f;
        for (size_t i = start; i < end; i++) {
            sum += data[i] * data[i];
        }
        return sqrtf(sum / static_cast<float>(end - start));
    }

    /// RMS of a slice of the 16 per-band values [start, end) — end is exclusive.
    static float rms_of_bands(const float* bands, int start, int end) {
        if (start >= end) return 0.0f;
        float sum = 0.0f;
        for (int i = start; i < end; i++) {
            sum += bands[i] * bands[i];
        }
        return sqrtf(sum / static_cast<float>(end - start));
    }
};

}  // namespace audio_reactive
}  // namespace esphome
