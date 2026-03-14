#include <cassert>
#include <cmath>
#include <cstdio>

#include "../../components/audio_reactive/band_aggregator.h"

using namespace esphome::audio_reactive;

// ── Helpers ──────────────────────────────────────────────────────────────────

static bool near(float a, float b, float tol = 1e-4f) {
    return fabsf(a - b) <= tol;
}

// ── 3-band (legacy) tests ─────────────────────────────────────────────────────

void test_aggregate_bass_only() {
    // With 43 Hz/bin (22050 Hz / 512 = ~43 Hz), bins 1-10 are sub-bass / bass.
    // BANDS_16 bands 0-3 cover bins 1-10 → those map to the bass summary.
    BandAggregator agg;
    float magnitudes[256] = {};
    // Energize only bins inside bands 0-3 (bin_start=1, band 3 ends at bin 11 exclusive)
    for (int i = 1; i <= 10; i++) {
        magnitudes[i] = 1.0f;
    }
    auto result = agg.aggregate(magnitudes, 256);
    assert(result.bass > 0.0f);
    assert(result.mid < result.bass);
    assert(result.high < result.bass);
    printf("PASS: test_aggregate_bass_only (bass=%.3f mid=%.3f high=%.3f)\n",
           result.bass, result.mid, result.high);
}

void test_aggregate_amplitude() {
    BandAggregator agg;
    float magnitudes[256] = {};
    for (int i = 0; i < 256; i++) {
        magnitudes[i] = 0.5f;
    }
    auto result = agg.aggregate(magnitudes, 256);
    assert(result.amplitude > 0.0f);
    // RMS of constant 0.5 values = 0.5 exactly
    assert(near(result.amplitude, 0.5f));
    printf("PASS: test_aggregate_amplitude (%.3f)\n", result.amplitude);
}

void test_aggregate_silence() {
    BandAggregator agg;
    float magnitudes[256] = {};
    auto result = agg.aggregate(magnitudes, 256);
    assert(result.bass == 0.0f);
    assert(result.mid == 0.0f);
    assert(result.high == 0.0f);
    assert(result.amplitude == 0.0f);
    printf("PASS: test_aggregate_silence\n");
}

// ── 16-band tests ─────────────────────────────────────────────────────────────

void test_aggregate16_all_zero() {
    BandAggregator agg;
    float magnitudes[256] = {};
    auto r = agg.aggregate16(magnitudes, 256);
    for (int b = 0; b < 16; b++) {
        assert(r.bands[b] == 0.0f);
    }
    assert(r.bass == 0.0f);
    assert(r.mid == 0.0f);
    assert(r.high == 0.0f);
    assert(r.amplitude == 0.0f);
    printf("PASS: test_aggregate16_all_zero\n");
}

void test_aggregate16_band_count() {
    // Sanity: all 16 band slots are populated (even if zero).
    BandAggregator agg;
    float magnitudes[256] = {};
    magnitudes[50] = 1.0f;  // band 9: bins 47-61
    auto r = agg.aggregate16(magnitudes, 256);
    assert(r.bands[9] > 0.0f);
    // Adjacent bands should be zero
    assert(r.bands[8] == 0.0f);
    assert(r.bands[10] == 0.0f);
    printf("PASS: test_aggregate16_band_count (band9=%.4f)\n", r.bands[9]);
}

void test_aggregate16_known_signal() {
    // Put unit energy into band 0 only (bins 1-2, end exclusive = bin 3).
    // RMS of 2 unit values = 1.0.
    BandAggregator agg;
    float magnitudes[256] = {};
    magnitudes[1] = 1.0f;
    magnitudes[2] = 1.0f;
    auto r = agg.aggregate16(magnitudes, 256);
    assert(near(r.bands[0], 1.0f));
    for (int b = 1; b < 16; b++) {
        assert(r.bands[b] == 0.0f);
    }
    // bass summary = RMS of bands 0-3 = sqrt(1^2 / 4) = 0.5
    assert(near(r.bass, 0.5f));
    assert(r.mid == 0.0f);
    assert(r.high == 0.0f);
    // Amplitude: only bins 1 and 2 are non-zero out of 255 bins
    // = sqrt((1+1) / 255)
    float expected_amp = sqrtf(2.0f / 255.0f);
    assert(near(r.amplitude, expected_amp, 1e-3f));
    printf("PASS: test_aggregate16_known_signal (band0=%.4f bass=%.4f amp=%.4f)\n",
           r.bands[0], r.bass, r.amplitude);
}

void test_aggregate16_summary_groupings() {
    // Energize one band in each summary region and confirm groupings.
    BandAggregator agg;
    float magnitudes[256] = {};

    // Band 2 (bins 5-7): bass region
    magnitudes[5] = magnitudes[6] = magnitudes[7] = 1.0f;
    // Band 6 (bins 20-26): mid region
    for (int i = 20; i < 27; i++) magnitudes[i] = 1.0f;
    // Band 12 (bins 102-130): high region
    for (int i = 102; i < 131; i++) magnitudes[i] = 1.0f;

    auto r = agg.aggregate16(magnitudes, 256);

    // All three summary bands should be non-zero
    assert(r.bass > 0.0f);
    assert(r.mid  > 0.0f);
    assert(r.high > 0.0f);
    // Bass/mid/high should be independent (no band straddles two summaries)
    // Bands 0-3 → bass, 4-9 → mid, 10-15 → high
    assert(r.bands[2] > 0.0f);  // in bass group
    assert(r.bands[6] > 0.0f);  // in mid group
    assert(r.bands[12] > 0.0f); // in high group
    printf("PASS: test_aggregate16_summary_groupings (bass=%.3f mid=%.3f high=%.3f)\n",
           r.bass, r.mid, r.high);
}

void test_aggregate16_amplitude_all_bins() {
    // Amplitude must cover all bins 1..bin_count-1.
    BandAggregator agg;
    float magnitudes[256] = {};
    // Energize a very high bin that falls outside all band definitions.
    // BANDS_16[15] ends at 256 (exclusive), so bin 255 is inside band 15.
    // Use a bin just to confirm amplitude picks it up.
    magnitudes[255] = 2.0f;
    auto r = agg.aggregate16(magnitudes, 256);
    // band 15 should capture it
    assert(r.bands[15] > 0.0f);
    // amplitude must also be > 0
    assert(r.amplitude > 0.0f);
    printf("PASS: test_aggregate16_amplitude_all_bins (amp=%.4f band15=%.4f)\n",
           r.amplitude, r.bands[15]);
}

void test_aggregate16_consistent_with_aggregate() {
    // aggregate() summary values must match aggregate16() summary values.
    BandAggregator agg;
    float magnitudes[256] = {};
    for (int i = 1; i < 256; i++) magnitudes[i] = static_cast<float>(i % 7) * 0.1f;

    auto r3  = agg.aggregate(magnitudes, 256);
    auto r16 = agg.aggregate16(magnitudes, 256);

    assert(near(r3.bass,      r16.bass));
    assert(near(r3.mid,       r16.mid));
    assert(near(r3.high,      r16.high));
    assert(near(r3.amplitude, r16.amplitude));
    printf("PASS: test_aggregate16_consistent_with_aggregate\n");
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    // Legacy 3-band tests
    test_aggregate_bass_only();
    test_aggregate_amplitude();
    test_aggregate_silence();

    // 16-band tests
    test_aggregate16_all_zero();
    test_aggregate16_band_count();
    test_aggregate16_known_signal();
    test_aggregate16_summary_groupings();
    test_aggregate16_amplitude_all_bins();
    test_aggregate16_consistent_with_aggregate();

    printf("All band aggregator tests passed.\n");
    return 0;
}
