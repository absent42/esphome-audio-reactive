#include <cassert>
#include <cmath>
#include <cstdio>

#include "../components/audio_reactive/agc.h"

using namespace audio_reactive;

void test_normalize_tracks_range() {
    AGC agc(10);
    for (int i = 1; i <= 10; i++) {
        agc.update(static_cast<float>(i));
    }
    float low = agc.normalize(1.0f);
    assert(low >= 0.0f && low <= 0.15f);
    float high = agc.normalize(10.0f);
    assert(high >= 0.85f && high <= 1.0f);
    printf("PASS: test_normalize_tracks_range (low=%.2f, high=%.2f)\n", low, high);
}

void test_normalize_clamps_output() {
    AGC agc(10);
    for (int i = 0; i < 10; i++) {
        agc.update(5.0f);
    }
    agc.update(10.0f);
    float result = agc.normalize(100.0f);
    assert(result == 1.0f);
    result = agc.normalize(-10.0f);
    assert(result == 0.0f);
    printf("PASS: test_normalize_clamps_output\n");
}

void test_range_adapts_over_time() {
    AGC agc(5);
    for (int i = 0; i < 5; i++) {
        agc.update(0.0f);
        agc.update(10.0f);
    }
    float mid_before = agc.normalize(5.0f);

    for (int i = 0; i < 10; i++) {
        agc.update(0.0f);
        agc.update(100.0f);
    }
    float mid_after = agc.normalize(5.0f);

    assert(mid_after < mid_before);
    printf("PASS: test_range_adapts_over_time (before=%.2f, after=%.2f)\n",
           mid_before, mid_after);
}

void test_no_division_by_zero() {
    AGC agc(10);
    for (int i = 0; i < 10; i++) {
        agc.update(5.0f);
    }
    float result = agc.normalize(5.0f);
    assert(result == 0.0f || result == 0.5f);
    printf("PASS: test_no_division_by_zero (result=%.2f)\n", result);
}

int main() {
    test_normalize_tracks_range();
    test_normalize_clamps_output();
    test_range_adapts_over_time();
    test_no_division_by_zero();
    printf("All AGC tests passed.\n");
    return 0;
}
