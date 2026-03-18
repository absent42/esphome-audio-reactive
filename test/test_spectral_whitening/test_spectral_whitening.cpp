#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../../components/audio_reactive/spectral_whitening.h"

using namespace esphome::audio_reactive;

static const size_t BINS = 256;
static const float UPDATE_RATE_HZ = 50.0f;

void test_flat_spectrum_unchanged() {
    SpectralWhitening<BINS> w(UPDATE_RATE_HZ);

    float mags[BINS];
    for (size_t i = 0; i < BINS; i++) mags[i] = 0.5f;

    // Warm up for 100 frames so peaks converge
    for (int frame = 0; frame < 100; frame++) {
        float buf[BINS];
        std::memcpy(buf, mags, sizeof(buf));
        w.process(buf, BINS);
    }

    // One final pass — capture output
    float out[BINS];
    std::memcpy(out, mags, sizeof(out));
    w.process(out, BINS);

    // Skip DC (bin 0), check bins 1..BINS-1 have similar values
    float min_val = out[1];
    float max_val = out[1];
    for (size_t i = 2; i < BINS; i++) {
        if (out[i] < min_val) min_val = out[i];
        if (out[i] > max_val) max_val = out[i];
    }
    assert(max_val > 0.0f);
    float ratio = min_val / max_val;
    assert(ratio > 0.8f);
    printf("PASS: test_flat_spectrum_unchanged (min/max ratio=%.3f)\n", ratio);
}

void test_dominant_bin_suppressed() {
    SpectralWhitening<BINS> w(UPDATE_RATE_HZ);

    // Warm up 100 frames: bin 50 at 10.0, others at 0.1
    float mags[BINS];
    for (size_t i = 0; i < BINS; i++) mags[i] = 0.1f;
    mags[50] = 10.0f;

    for (int frame = 0; frame < 100; frame++) {
        float buf[BINS];
        std::memcpy(buf, mags, sizeof(buf));
        w.process(buf, BINS);
    }

    // Check whitened output: bin 50 ratio to neighbors should be < 10 (was 100:1)
    float out[BINS];
    std::memcpy(out, mags, sizeof(out));
    w.process(out, BINS);

    float neighbor_avg = (out[49] + out[51]) * 0.5f;
    assert(neighbor_avg > 0.0f);
    float ratio = out[50] / neighbor_avg;
    assert(ratio < 10.0f);
    printf("PASS: test_dominant_bin_suppressed (bin50/neighbor ratio=%.2f)\n", ratio);
}

void test_transient_passes_through() {
    SpectralWhitening<BINS> w(UPDATE_RATE_HZ);

    // Warm up with uniform spectrum
    float uniform[BINS];
    for (size_t i = 0; i < BINS; i++) uniform[i] = 0.2f;

    for (int frame = 0; frame < 100; frame++) {
        float buf[BINS];
        std::memcpy(buf, uniform, sizeof(buf));
        w.process(buf, BINS);
    }

    // Sudden spike in bin 100 — whitening should not yet have suppressed it
    float spike[BINS];
    std::memcpy(spike, uniform, sizeof(spike));
    spike[100] = 2.0f;  // 10x the neighbors after whitening adapts; here it's sudden

    float out[BINS];
    std::memcpy(out, spike, sizeof(out));
    w.process(out, BINS);

    float neighbor_avg = (out[99] + out[101]) * 0.5f;
    assert(neighbor_avg > 0.0f);
    float ratio = out[100] / neighbor_avg;
    // Transient should still be prominent: > 5x neighbors
    assert(ratio > 5.0f);
    printf("PASS: test_transient_passes_through (spike/neighbor ratio=%.2f)\n", ratio);
}

void test_reset() {
    SpectralWhitening<BINS> w(UPDATE_RATE_HZ);

    // Build up peaks with large values
    float mags[BINS];
    for (size_t i = 0; i < BINS; i++) mags[i] = 1.0f;

    for (int frame = 0; frame < 50; frame++) {
        float buf[BINS];
        std::memcpy(buf, mags, sizeof(buf));
        w.process(buf, BINS);
    }

    // Reset and verify no crash, outputs are >= 0
    w.reset();

    float buf[BINS];
    std::memcpy(buf, mags, sizeof(buf));
    w.process(buf, BINS);

    for (size_t i = 1; i < BINS; i++) {
        assert(buf[i] >= 0.0f);
    }
    printf("PASS: test_reset\n");
}

int main() {
    test_flat_spectrum_unchanged();
    test_dominant_bin_suppressed();
    test_transient_passes_through();
    test_reset();
    printf("All spectral whitening tests passed.\n");
    return 0;
}
