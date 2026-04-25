#pragma once

// Aggregation layer: 32 mel bands -> 7 perceptual musical bands.
// Owns a per-band AGC for normalization (same algorithm as basic tier).
//
// Consumes RAW (pre-log) mel energies and aggregates in the linear/energy domain
// ("total energy in band = sum of bin energies"). This avoids geometric-mean
// artifacts that would arise from log-domain averaging.

#include <cmath>
#include <cstdint>

#if defined(AUDIO_REACTIVE_NATIVE_TEST) && !defined(MUSICAL_BANDS_USE_REAL_AGC)
// Minimal AGC stub for unit-test native compilation that doesn't care about
// AGC dynamics (e.g. test_musical_bands which only verifies mel-slot
// aggregation correctness). Mirrors the public surface used by MusicalBands
// (default ctor, set_noise_floor, process, reset).
//
// Define MUSICAL_BANDS_USE_REAL_AGC before including this header to use the
// real AGC class even under native test (e.g. test_e2e_pro_pipeline, where
// AGC dynamics are exactly what we want to exercise).
enum AGCMode { AGC_NORMAL };
struct AGC_Stub {
    AGC_Stub(AGCMode mode = AGC_NORMAL) { (void) mode; }
    float noise_floor{0.0f};
    void set_noise_floor(float n) { noise_floor = n; }
    float process(float input) { return input; }  // passthrough
    void reset() {}
};
#else
#include "agc.h"
#endif

namespace esphome {
namespace audio_reactive {

/// 7-band perceptual musical-bands aggregator.
/// Operates on RAW (pre-log) mel energies — sums in the linear domain,
/// then applies per-band AGC and asymmetric EMA smoothing.
class MusicalBands {
 public:
    static constexpr uint8_t kNumBands = 7;
    enum Band : uint8_t {
        SUB_BASS = 0,
        BASS = 1,
        LOW_MID = 2,
        MID = 3,
        UPPER_MID = 4,
        HIGH = 5,
        AIR = 6,
    };

    // Fixed mel-band index mappings (inclusive start, exclusive end).
    // Tuned for N_MEL=32 with freq_min=40, freq_max=16000 per design spec.
    static constexpr uint8_t kMelStart[kNumBands] = { 0,  1,  4,  8, 15, 20, 26};
    static constexpr uint8_t kMelEnd  [kNumBands] = { 1,  4,  8, 15, 20, 26, 32};

    // EMA coefficients — must match basic-tier audio_reactive.h values.
    // If basic-tier constants change, update these (or factor to a shared header).
    static constexpr float kEmaFastRise = 0.75f;
    static constexpr float kEmaSlowFall = 0.17f;

#if defined(AUDIO_REACTIVE_NATIVE_TEST) && !defined(MUSICAL_BANDS_USE_REAL_AGC)
    using AGCType = AGC_Stub;
#else
    using AGCType = AGC;
#endif

    /// Compute the 7-band energies from a 32-band RAW (pre-log) mel frame.
    /// Input: N_MEL = 32 raw energies (NOT log-compressed).
    /// Output: 7 normalized band energies in [0, 1].
    void process(const float *mel_bands_raw, float *out_bands) {
        for (uint8_t m = 0; m < kNumBands; m++) {
            // Sum raw energies across the band's mel slice (linear-domain mixing).
            float energy = 0.0f;
            for (uint8_t i = kMelStart[m]; i < kMelEnd[m]; i++) {
                energy += mel_bands_raw[i];
            }
            float agc_out = agcs_[m].process(energy);

            // Asymmetric EMA (fast attack, slow decay)
            float prev = smoothed_[m];
            float alpha = (agc_out > prev) ? kEmaFastRise : kEmaSlowFall;
            smoothed_[m] = alpha * agc_out + (1.0f - alpha) * prev;
            out_bands[m] = smoothed_[m];
        }
    }

    /// Set per-band noise floor from V2 calibration data.
    /// `noise_floor_per_band` must point to 7 floats.
    void set_noise_floors(const float *noise_floor_per_band) {
        for (uint8_t m = 0; m < kNumBands; m++) {
            agcs_[m].set_noise_floor(noise_floor_per_band[m]);
        }
    }

    /// Reset all AGC states and smoothing history.
    void reset() {
        for (auto &a : agcs_) a.reset();
        for (auto &s : smoothed_) s = 0.0f;
    }

    const AGCType &agc(uint8_t band) const { return agcs_[band]; }

 protected:
    // Note: no `{}` brace-init on agcs_. AGC's only constructor is marked
    // `explicit AGC(AGCPreset = AGC_NORMAL)`, and value-init via `{}` would
    // emit a "converting from initializer list" warning on every translation
    // unit that includes this header. Default-init (no braces) still calls
    // the default constructor, just without the warning.
    AGCType agcs_[kNumBands];
    float smoothed_[kNumBands]{};
};

}  // namespace audio_reactive
}  // namespace esphome
