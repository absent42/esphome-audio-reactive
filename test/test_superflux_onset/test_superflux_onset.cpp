// test/test_superflux_onset/test_superflux_onset.cpp
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#define AUDIO_REACTIVE_NATIVE_TEST
#include "../../components/audio_reactive/superflux_onset.h"

using namespace esphome::audio_reactive;
using SF = SuperFluxOnset<32>;

void test_reset_and_idle() {
    SF sf;
    sf.reset();
    float mel[32] = {};
    for (int i = 0; i < 10; i++) {
        auto r = sf.process(mel);
        (void)r;  // All zero input — no onsets, no crashes.
    }
    printf("PASS: test_reset_and_idle\n");
}

void test_clean_transient_fires_onset() {
    SF sf;
    sf.reset();
    float low[32]; for (int i = 0; i < 32; i++) low[i] = 0.001f;
    float hi[32];  for (int i = 0; i < 32; i++) hi[i] = 10.0f;

    // Warm up peak-picker history with low.
    bool any_event = false;
    for (int k = 0; k < 20; k++) {
        auto r = sf.process(low);
        if (r.event) any_event = true;
    }
    assert(!any_event);

    // Sudden transient: feed high mel for 1 frame, then low again.
    sf.process(hi);
    for (int k = 0; k < 5; k++) {
        auto r = sf.process(low);
        if (r.event) any_event = true;
    }
    assert(any_event);
    printf("PASS: test_clean_transient_fires_onset\n");
}

void test_sustained_tone_no_onset() {
    SF sf;
    sf.reset();
    float steady[32]; for (int i = 0; i < 32; i++) steady[i] = 1.0f + 0.01f * sinf(i * 0.5f);
    bool any_event = false;
    for (int k = 0; k < 50; k++) {
        auto r = sf.process(steady);
        if (r.event) any_event = true;
    }
    // Slight wobble should be suppressed by max-filter and threshold.
    assert(!any_event);
    printf("PASS: test_sustained_tone_no_onset\n");
}

void test_min_interval_suppresses_doubles() {
    SF sf;
    sf.reset();
    float low[32]; for (int i = 0; i < 32; i++) low[i] = 0.001f;
    float hi[32];  for (int i = 0; i < 32; i++) hi[i] = 10.0f;

    for (int k = 0; k < 20; k++) sf.process(low);

    // Alternate hi/low for several frames: only 1 onset per min-interval.
    int onset_count = 0;
    for (int k = 0; k < 10; k++) {
        auto r = sf.process(hi);
        if (r.event) onset_count++;
        auto r2 = sf.process(low);
        if (r2.event) onset_count++;
    }
    // Minimum inter-onset is 3 frames; over 20 frames, max 7 onsets.
    assert(onset_count <= 7);
    printf("PASS: test_min_interval_suppresses_doubles (count=%d)\n", onset_count);
}

int main() {
    test_reset_and_idle();
    test_clean_transient_fires_onset();
    test_sustained_tone_no_onset();
    test_min_interval_suppresses_doubles();
    printf("ALL SUPERFLUX TESTS PASSED\n");
    return 0;
}
