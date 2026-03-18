#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../../components/audio_reactive/spectral_descriptors.h"

using namespace esphome::audio_reactive;

// ── Helpers ──────────────────────────────────────────────────────────────────

static bool near(float a, float b, float tol = 1.0f) {
    return fabsf(a - b) <= tol;
}

// ── Tests ─────────────────────────────────────────────────────────────────────

// All energy in a single bin — centroid must land at that bin's frequency.
void test_centroid_single_bin() {
    const int bin_count = 256;
    const float hz_per_bin = 43.066f;
    float mags[256] = {};
    mags[10] = 1.0f;

    float centroid = spectral_centroid(mags, bin_count, hz_per_bin);
    float expected = 10 * hz_per_bin;

    assert(near(centroid, expected, 0.1f));
    printf("PASS: test_centroid_single_bin (centroid=%.2f expected=%.2f)\n",
           centroid, expected);
}

// Uniform spectrum — centroid should land near the midpoint of bins 1..255.
void test_centroid_flat_spectrum() {
    const int bin_count = 256;
    const float hz_per_bin = 43.066f;
    float mags[256];
    for (int i = 0; i < bin_count; i++) mags[i] = 1.0f;

    float centroid = spectral_centroid(mags, bin_count, hz_per_bin);
    // Midpoint of bins 1..255 = bin 128 = 128 * hz_per_bin
    float expected = 128.0f * hz_per_bin;

    // Allow a few bins of tolerance
    assert(near(centroid, expected, 3.0f * hz_per_bin));
    printf("PASS: test_centroid_flat_spectrum (centroid=%.2f expected~=%.2f)\n",
           centroid, expected);
}

// Silent spectrum — centroid must return 0.
void test_centroid_silence() {
    const int bin_count = 256;
    const float hz_per_bin = 43.066f;
    float mags[256] = {};

    float centroid = spectral_centroid(mags, bin_count, hz_per_bin);
    assert(centroid == 0.0f);
    printf("PASS: test_centroid_silence\n");
}

// Energy only in bins 1-10 — rolloff must be at or below bin 11's frequency.
void test_rolloff_all_low() {
    const int bin_count = 256;
    const float hz_per_bin = 43.066f;
    float mags[256] = {};
    for (int i = 1; i <= 10; i++) mags[i] = 1.0f;

    float rolloff = spectral_rolloff(mags, bin_count, hz_per_bin, 0.85f);
    float limit = 11.0f * hz_per_bin;

    assert(rolloff <= limit);
    printf("PASS: test_rolloff_all_low (rolloff=%.2f limit=%.2f)\n",
           rolloff, limit);
}

// Uniform spectrum — rolloff should be near 85% of the total frequency range.
void test_rolloff_flat_spectrum() {
    const int bin_count = 256;
    const float hz_per_bin = 43.066f;
    float mags[256];
    for (int i = 0; i < bin_count; i++) mags[i] = 1.0f;

    float rolloff = spectral_rolloff(mags, bin_count, hz_per_bin, 0.85f);
    // With uniform energy, 85% of bins 1..255 = bin ~217
    float expected = 217.0f * hz_per_bin;

    // Allow a few bins of tolerance
    assert(near(rolloff, expected, 5.0f * hz_per_bin));
    printf("PASS: test_rolloff_flat_spectrum (rolloff=%.2f expected~=%.2f)\n",
           rolloff, expected);
}

// Silent spectrum — rolloff must return 0.
void test_rolloff_silence() {
    const int bin_count = 256;
    const float hz_per_bin = 43.066f;
    float mags[256] = {};

    float rolloff = spectral_rolloff(mags, bin_count, hz_per_bin, 0.85f);
    assert(rolloff == 0.0f);
    printf("PASS: test_rolloff_silence\n");
}

// Low-freq energy → low centroid; high-freq energy → high centroid.
void test_centroid_normalized() {
    const int bin_count = 256;
    const float hz_per_bin = 43.066f;

    float mags_low[256] = {};
    float mags_high[256] = {};

    // Low: energy in bins 1-20
    for (int i = 1; i <= 20; i++) mags_low[i] = 1.0f;
    // High: energy in bins 200-255
    for (int i = 200; i < 256; i++) mags_high[i] = 1.0f;

    float centroid_low  = spectral_centroid(mags_low,  bin_count, hz_per_bin);
    float centroid_high = spectral_centroid(mags_high, bin_count, hz_per_bin);

    assert(centroid_low > 0.0f);
    assert(centroid_high > 0.0f);
    assert(centroid_low < centroid_high);
    printf("PASS: test_centroid_normalized (low=%.2f high=%.2f)\n",
           centroid_low, centroid_high);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    test_centroid_single_bin();
    test_centroid_flat_spectrum();
    test_centroid_silence();
    test_rolloff_all_low();
    test_rolloff_flat_spectrum();
    test_rolloff_silence();
    test_centroid_normalized();

    printf("All spectral descriptor tests passed.\n");
    return 0;
}
