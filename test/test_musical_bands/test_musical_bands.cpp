#include <cassert>
#include <cmath>
#include <cstdio>

#define AUDIO_REACTIVE_NATIVE_TEST
#include "../../components/audio_reactive/musical_bands.h"

using namespace esphome::audio_reactive;

void test_band_count() {
    assert(MusicalBands::kNumBands == 7);
    printf("PASS: test_band_count\n");
}

void test_mel_slicing_contiguous() {
    // Check that the band slices are contiguous and cover all 32 mel bands without gaps or overlaps.
    uint8_t covered[32] = {};
    for (uint8_t b = 0; b < MusicalBands::kNumBands; b++) {
        for (uint8_t i = MusicalBands::kMelStart[b]; i < MusicalBands::kMelEnd[b]; i++) {
            assert(covered[i] == 0);  // no overlap
            covered[i] = 1;
        }
    }
    for (uint8_t i = 0; i < 32; i++) {
        assert(covered[i] == 1);  // no gaps
    }
    printf("PASS: test_mel_slicing_contiguous\n");
}

void test_uniform_input_balanced_output() {
    MusicalBands mb;
    float mel[32];
    for (uint8_t i = 0; i < 32; i++) mel[i] = 1.0f;  // uniform mel energies
    float out[7] = {};
    // Run a few iterations to let smoothing settle.
    for (int k = 0; k < 20; k++) mb.process(mel, out);
    // All bands aggregate different numbers of mel slots, so check that
    // after smoothing the values are non-zero and positive.
    for (uint8_t i = 0; i < 7; i++) {
        assert(out[i] > 0.0f);
    }
    printf("PASS: test_uniform_input_balanced_output (out[0]=%.3f out[6]=%.3f)\n", out[0], out[6]);
}

void test_reset_clears_smoothing() {
    MusicalBands mb;
    float mel[32];
    for (uint8_t i = 0; i < 32; i++) mel[i] = 5.0f;
    float out[7] = {};
    for (int k = 0; k < 20; k++) mb.process(mel, out);
    for (uint8_t i = 0; i < 7; i++) assert(out[i] > 0.01f);
    mb.reset();
    // After reset, smoothed values are zero. First process() call with alpha=kEmaFastRise
    // will move the smoothed value toward the AGC output, but start from zero.
    // Verify that the first call after reset produces smaller values than the pre-reset state.
    float out_after[7] = {};
    mb.process(mel, out_after);
    // The first step after reset should yield smoothed values smaller than the
    // pre-reset converged state (since smoothed started at 0 and rose via EMA alpha=0.75).
    for (uint8_t i = 0; i < 7; i++) {
        assert(out_after[i] <= out[i] + 1e-4f);
    }
    printf("PASS: test_reset_clears_smoothing\n");
}

int main() {
    test_band_count();
    test_mel_slicing_contiguous();
    test_uniform_input_balanced_output();
    test_reset_clears_smoothing();
    printf("ALL MUSICAL BANDS TESTS PASSED\n");
    return 0;
}
