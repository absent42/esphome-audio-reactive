// Native-host test for V1 -> V2 calibration struct migration.
// Uses the shared calibration_migration.h header directly - no mirror structs,
// no drift possible.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>

// Tolerance helper for float midpoint comparisons (single-precision round-off).
static inline bool approx_eq(float a, float b, float eps = 1e-6f) {
    return std::fabs(a - b) <= eps;
}

// Both flags are required: AUDIO_REACTIVE_PRO enables the V2 struct + migrate
// function; AUDIO_REACTIVE_NATIVE_TEST is a marker for any future conditional
// guards (currently unused by calibration_migration.h but kept for parity with
// the test_fft_processor pattern).
#define AUDIO_REACTIVE_PRO
#define AUDIO_REACTIVE_NATIVE_TEST

#include "../../components/audio_reactive/calibration_migration.h"

using namespace esphome::audio_reactive;

void test_monotonic_input() {
    CalibrationStore v1 = {2.5f, 0.30f, 0.10f, 0.02f, 0.15f, 0.05f, true, true};
    CalibrationStoreV2 v2 = {};
    assert(migrate_v1_to_v2(v1, v2));

    assert(v2.version == 2);
    assert(v2.squelch_threshold == 2.5f);
    assert(v2.raw_scale == 0.05f);
    assert(v2.quiet_calibrated == true);
    assert(v2.music_calibrated == true);

    assert(v2.noise_floor[0] == 0.30f);          // sub_bass (copy)
    assert(v2.noise_floor[1] == 0.30f);          // bass (copy)
    assert(approx_eq(v2.noise_floor[2], 0.20f)); // low_mid (interpolated)
    assert(v2.noise_floor[3] == 0.10f);          // mid (copy)
    assert(approx_eq(v2.noise_floor[4], 0.06f)); // upper_mid (interpolated)
    assert(v2.noise_floor[5] == 0.02f);          // high (copy)
    assert(v2.noise_floor[6] == 0.02f);          // air (copy)

    for (int i = 1; i < 7; i++) {
        assert(v2.noise_floor[i] <= v2.noise_floor[i - 1]);
    }
    printf("PASS: test_monotonic_input\n");
}

void test_zero_input() {
    CalibrationStore v1 = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.05f, false, false};
    CalibrationStoreV2 v2 = {};
    assert(migrate_v1_to_v2(v1, v2));

    assert(v2.version == 2);
    assert(v2.squelch_threshold == 0.0f);
    assert(v2.quiet_calibrated == false);
    assert(v2.music_calibrated == false);
    for (int i = 0; i < 7; i++) {
        assert(v2.noise_floor[i] == 0.0f);
    }
    printf("PASS: test_zero_input\n");
}

void test_nonmonotonic_input() {
    // Edge case: mid > bass (shouldn't happen in practice but must not crash)
    CalibrationStore v1 = {1.0f, 0.1f, 0.4f, 0.2f, 0.15f, 0.05f, true, false};
    CalibrationStoreV2 v2 = {};
    assert(migrate_v1_to_v2(v1, v2));

    assert(approx_eq(v2.noise_floor[2], 0.25f));  // (0.1 + 0.4) / 2
    assert(approx_eq(v2.noise_floor[4], 0.30f));  // (0.4 + 0.2) / 2
    printf("PASS: test_nonmonotonic_input\n");
}

void test_struct_size_reasonable() {
    assert(sizeof(CalibrationStoreV2) <= 64);
    printf("PASS: test_struct_size_reasonable (sizeof = %zu)\n", sizeof(CalibrationStoreV2));
}

void test_both_structs_distinct_sizes() {
    // Just a sanity check that V1 and V2 are the layouts we expect.
    assert(sizeof(CalibrationStore) > 0);
    assert(sizeof(CalibrationStoreV2) > sizeof(CalibrationStore));
    printf("PASS: test_both_structs_distinct_sizes (V1=%zu, V2=%zu)\n",
           sizeof(CalibrationStore), sizeof(CalibrationStoreV2));
}

int main() {
    test_monotonic_input();
    test_zero_input();
    test_nonmonotonic_input();
    test_struct_size_reasonable();
    test_both_structs_distinct_sizes();
    printf("ALL CALIBRATION MIGRATION TESTS PASSED\n");
    return 0;
}
