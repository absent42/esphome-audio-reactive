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

// ── Dynamic sample rate / FFT size tests ─────────────────────────────────────

void test_aggregate16_default_matches_22050() {
    // Default constructor must produce identical bin mappings to explicit (22050, 512).
    BandAggregator agg_default;
    BandAggregator agg_explicit(22050.0f, 512);

    // Verify hz_per_bin matches
    assert(near(agg_default.hz_per_bin(), agg_explicit.hz_per_bin()));

    // Verify all 16 band definitions are identical
    for (int b = 0; b < 16; b++) {
        assert(agg_default.band(b).bin_start == agg_explicit.band(b).bin_start);
        assert(agg_default.band(b).bin_end == agg_explicit.band(b).bin_end);
    }

    // Verify identical output on a non-trivial signal
    float magnitudes[256] = {};
    for (int i = 1; i < 256; i++) magnitudes[i] = static_cast<float>(i % 11) * 0.05f;

    auto r_default  = agg_default.aggregate16(magnitudes, 256);
    auto r_explicit = agg_explicit.aggregate16(magnitudes, 256);

    for (int b = 0; b < 16; b++) {
        assert(near(r_default.bands[b], r_explicit.bands[b]));
    }
    assert(near(r_default.bass, r_explicit.bass));
    assert(near(r_default.mid, r_explicit.mid));
    assert(near(r_default.high, r_explicit.high));
    assert(near(r_default.amplitude, r_explicit.amplitude));

    printf("PASS: test_aggregate16_default_matches_22050\n");
}

void test_aggregate16_44100hz_1024fft() {
    // 44100/1024 has the same hz_per_bin (43.066 Hz) as 22050/512,
    // so bin mappings must be identical.
    BandAggregator agg_ref(22050.0f, 512);
    BandAggregator agg_44k(44100.0f, 1024);

    assert(near(agg_ref.hz_per_bin(), agg_44k.hz_per_bin()));

    for (int b = 0; b < 16; b++) {
        assert(agg_ref.band(b).bin_start == agg_44k.band(b).bin_start);
        assert(agg_ref.band(b).bin_end == agg_44k.band(b).bin_end);
    }

    // Run on a signal and verify identical results (same bin count = 256 for both 512/2)
    // Note: 1024/2 = 512 bins, but we only use 256 to match the reference signal size.
    float magnitudes[256] = {};
    for (int i = 1; i < 256; i++) magnitudes[i] = static_cast<float>(i % 7) * 0.1f;

    auto r_ref = agg_ref.aggregate16(magnitudes, 256);
    auto r_44k = agg_44k.aggregate16(magnitudes, 256);

    for (int b = 0; b < 16; b++) {
        assert(near(r_ref.bands[b], r_44k.bands[b]));
    }
    assert(near(r_ref.bass, r_44k.bass));
    assert(near(r_ref.mid, r_44k.mid));
    assert(near(r_ref.high, r_44k.high));
    assert(near(r_ref.amplitude, r_44k.amplitude));

    printf("PASS: test_aggregate16_44100hz_1024fft\n");
}

void test_aggregate16_48000hz_1024fft() {
    // 48000/1024 = 46.875 Hz/bin — different from 22050/512.
    // Verify the bin mappings are computed correctly from frequency boundaries.
    BandAggregator agg(48000.0f, 1024);

    assert(near(agg.hz_per_bin(), 46.875f));

    // Expected bin mappings for 48000/1024 (46.875 Hz/bin):
    // freq_to_bin(f) = round(f / 46.875)
    // Boundary freqs → expected bins:
    //   43.066 →  0.919 → 1      129.199 →  2.756 → 3
    //  215.332 →  4.594 → 5      344.531 →  7.350 → 7
    //  473.730 → 10.106 → 10     645.996 → 13.781 → 14
    //  861.328 → 18.375 → 18    1162.793 → 24.806 → 25
    // 1550.391 → 33.075 → 33    2024.121 → 43.181 → 43
    // 2627.051 → 56.044 → 56    3402.246 → 72.581 → 73
    // 4392.773 → 93.713 → 94    5641.699 → 120.356 → 120
    // 7278.223 → 155.269 → 155   9388.477 → 200.288 → 200
    // 11025.0  → 235.200 → 235
    size_t expected_starts[16] = {1, 3, 5, 7, 10, 14, 18, 25, 33, 43, 56, 73, 94, 120, 155, 200};
    size_t expected_ends[16]   = {3, 5, 7, 10, 14, 18, 25, 33, 43, 56, 73, 94, 120, 155, 200, 235};

    for (int b = 0; b < 16; b++) {
        if (agg.band(b).bin_start != expected_starts[b] ||
            agg.band(b).bin_end != expected_ends[b]) {
            printf("FAIL: band %d: expected [%zu, %zu) got [%zu, %zu)\n",
                   b, expected_starts[b], expected_ends[b],
                   agg.band(b).bin_start, agg.band(b).bin_end);
            assert(false);
        }
    }

    // Verify the aggregator works: energize a bin in band 5 (bins 14-17) and check
    float magnitudes[512] = {};
    magnitudes[15] = 1.0f;  // inside band 5 [14, 18)
    auto r = agg.aggregate16(magnitudes, 512);
    assert(r.bands[5] > 0.0f);
    // Adjacent bands should be zero
    assert(r.bands[4] == 0.0f);
    assert(r.bands[6] == 0.0f);
    // Band 5 is in the mid group (bands 4-9)
    assert(r.mid > 0.0f);

    printf("PASS: test_aggregate16_48000hz_1024fft (band5=%.4f mid=%.4f)\n",
           r.bands[5], r.mid);
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

    // Dynamic sample rate / FFT size tests
    test_aggregate16_default_matches_22050();
    test_aggregate16_44100hz_1024fft();
    test_aggregate16_48000hz_1024fft();

    printf("All band aggregator tests passed.\n");
    return 0;
}
