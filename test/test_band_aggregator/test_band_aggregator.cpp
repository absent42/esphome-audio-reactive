#include <cassert>
#include <cmath>
#include <cstdio>

#include "../../components/audio_reactive/band_aggregator.h"

using namespace esphome::audio_reactive;

void test_band_ranges() {
    BandAggregator agg(31.25f);
    auto ranges = agg.band_bin_ranges(256);
    assert(ranges.bass_start >= 1);
    assert(ranges.bass_end <= 12);
    assert(ranges.mid_start > ranges.bass_end);
    assert(ranges.high_start > ranges.mid_end);
    printf("PASS: test_band_ranges\n");
}

void test_aggregate_bass_only() {
    BandAggregator agg(31.25f);
    float magnitudes[256] = {};
    for (int i = 2; i <= 10; i++) {
        magnitudes[i] = 1.0f;
    }
    auto result = agg.aggregate(magnitudes, 256);
    assert(result.bass > 0.0f);
    assert(result.mid < result.bass);
    assert(result.high < result.bass);
    printf("PASS: test_aggregate_bass_only\n");
}

void test_aggregate_amplitude() {
    BandAggregator agg(31.25f);
    float magnitudes[256] = {};
    for (int i = 0; i < 256; i++) {
        magnitudes[i] = 0.5f;
    }
    auto result = agg.aggregate(magnitudes, 256);
    assert(result.amplitude > 0.0f);
    assert(result.amplitude <= 1.0f);
    printf("PASS: test_aggregate_amplitude (%.3f)\n", result.amplitude);
}

void test_aggregate_silence() {
    BandAggregator agg(31.25f);
    float magnitudes[256] = {};
    auto result = agg.aggregate(magnitudes, 256);
    assert(result.bass == 0.0f);
    assert(result.mid == 0.0f);
    assert(result.high == 0.0f);
    assert(result.amplitude == 0.0f);
    printf("PASS: test_aggregate_silence\n");
}

int main() {
    test_band_ranges();
    test_aggregate_bass_only();
    test_aggregate_amplitude();
    test_aggregate_silence();
    printf("All band aggregator tests passed.\n");
    return 0;
}
