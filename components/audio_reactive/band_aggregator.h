#pragma once

#include <cmath>
#include <cstddef>

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

struct BandDefinition {
    size_t bin_start;
    size_t bin_end;  // exclusive
};

/// 16-band definitions adapted from WLED for 22,050 Hz sample rate (~43 Hz/bin).
static constexpr BandDefinition BANDS_16[16] = {
    {1,   3},    // 0:  43-129 Hz   (sub-bass)
    {3,   5},    // 1:  129-215 Hz  (bass)
    {5,   8},    // 2:  215-344 Hz  (bass)
    {8,   11},   // 3:  344-473 Hz  (bass/mid)
    {11,  15},   // 4:  473-645 Hz  (mid)
    {15,  20},   // 5:  645-860 Hz  (mid)
    {20,  27},   // 6:  860-1161 Hz (mid)
    {27,  36},   // 7:  1161-1548 Hz(mid)
    {36,  47},   // 8:  1548-2021 Hz(mid)
    {47,  61},   // 9:  2021-2623 Hz(mid/high)
    {61,  79},   // 10: 2623-3397 Hz(high-mid)
    {79,  102},  // 11: 3397-4386 Hz(high-mid)
    {102, 131},  // 12: 4386-5634 Hz(high)
    {131, 169},  // 13: 5634-7268 Hz(high)
    {169, 218},  // 14: 7268-9374 Hz(high)
    {218, 256},  // 15: 9374-11008 Hz(high)
};

/// Groups FFT magnitude bins into 16 bands plus bass/mid/high summary energies.
class BandAggregator {
 public:
    BandAggregator() = default;

    /// Aggregate FFT magnitudes into 16-band energies with summary values.
    /// @param magnitudes  FFT magnitude bins (index 0 = DC bin, skipped).
    /// @param bin_count   Number of bins in the magnitudes array.
    BandEnergies16 aggregate16(const float* magnitudes, size_t bin_count) const {
        BandEnergies16 result{};

        for (int b = 0; b < 16; b++) {
            size_t start = BANDS_16[b].bin_start;
            size_t end = BANDS_16[b].bin_end;  // exclusive
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

 private:
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
