#pragma once

// Single source of truth for calibration struct layouts and migration logic.
// Included from audio_reactive.h (device build) and test_calibration_migration
// (native build). Native tests must not pull in ESPHome-specific headers, so
// this file is deliberately header-only with only cstdint.

#include <cstdint>

namespace esphome {
namespace audio_reactive {

/// V1 (basic-tier) persistent calibration layout. Unchanged from original design.
struct CalibrationStore {
    float squelch_threshold;    // From quiet cal: max_mid_high * 1.5
    float noise_floor_bass;     // Pre-scaled quiet max bass
    float noise_floor_mid;
    float noise_floor_high;
    float noise_floor_amp;
    float raw_scale;            // From music cal: 0.5 / avg_amplitude
    bool quiet_calibrated;
    bool music_calibrated;
};

#ifdef AUDIO_REACTIVE_PRO
/// V2 (pro-tier) persistent calibration layout.
/// 7 per-musical-band noise floors (sub_bass..air) replace V1's 4 scalars.
/// Amp is derived from mean across bands at runtime - not stored.
struct CalibrationStoreV2 {
    uint8_t version;             // = 2
    uint8_t reserved[3];
    float squelch_threshold;     // Max musical-band quiet-mean * 1.5
    float noise_floor[7];        // Per musical band (sub_bass..air), post-scale
    float raw_scale;             // 0.5 / music_avg_amplitude
    bool quiet_calibrated;
    bool music_calibrated;
    uint8_t _pad[2];
};

static_assert(sizeof(CalibrationStoreV2) <= 64, "V2 struct must fit in a single NVS entry");

/// Migrate V1 CalibrationStore to V2 by linear-interpolating 3-band noise floors
/// across 7 musical bands. Pure function - no I/O, no ESPHome deps.
///
/// Musical band index mapping:
///   0: sub_bass (~= V1 bass)
///   1: bass     (V1 bass)
///   2: low_mid  ((V1 bass + V1 mid) / 2)
///   3: mid      (V1 mid)
///   4: upper_mid((V1 mid + V1 high) / 2)
///   5: high     (V1 high)
///   6: air      (~= V1 high)
///
/// V1's noise_floor_amp is not carried forward - V2 derives amp from the mean
/// across musical bands at runtime. Add a // V3-migration-goes-here comment
/// if a future version supersedes this.
inline bool migrate_v1_to_v2(const CalibrationStore &v1, CalibrationStoreV2 &v2) {
    v2.version = 2;
    v2.squelch_threshold = v1.squelch_threshold;
    v2.raw_scale = v1.raw_scale;
    v2.quiet_calibrated = v1.quiet_calibrated;
    v2.music_calibrated = v1.music_calibrated;

    v2.noise_floor[0] = v1.noise_floor_bass;
    v2.noise_floor[1] = v1.noise_floor_bass;
    v2.noise_floor[2] = 0.5f * (v1.noise_floor_bass + v1.noise_floor_mid);
    v2.noise_floor[3] = v1.noise_floor_mid;
    v2.noise_floor[4] = 0.5f * (v1.noise_floor_mid + v1.noise_floor_high);
    v2.noise_floor[5] = v1.noise_floor_high;
    v2.noise_floor[6] = v1.noise_floor_high;
    return true;
}
#endif  // AUDIO_REACTIVE_PRO

}  // namespace audio_reactive
}  // namespace esphome
