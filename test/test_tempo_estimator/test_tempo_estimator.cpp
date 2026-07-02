// Unit tests for TempoEstimator. Native build:
//   g++ -std=c++17 -O2 -I components/audio_reactive \
//       test/test_tempo_estimator/test_tempo_estimator.cpp \
//       -o /tmp/test_tempo_estimator && /tmp/test_tempo_estimator
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "tempo_estimator.h"

using esphome::audio_reactive::TempoEstimator;

void test_grid_constants_consistent() {
    // 60..180 inclusive in 1-BPM steps = 121 candidates.
    assert(TempoEstimator::kNumCandidates ==
           (int)(TempoEstimator::kBpmMax - TempoEstimator::kBpmMin) + 1);
    printf("PASS: test_grid_constants_consistent\n");
}

void test_adaptive_threshold_constant_dc_becomes_zero() {
    constexpr int N = 32;
    float x[N], scratch[N];
    for (int i = 0; i < N; i++) x[i] = 5.0f;
    TempoEstimator::adaptive_threshold(x, N, scratch);
    for (int i = 0; i < N; i++) {
        // Local mean of a constant signal is the constant. Subtract → 0.
        // Clamp doesn't change a 0. So output is exactly 0 for all i.
        if (x[i] != 0.0f) {
            fprintf(stderr,
                    "FAIL: test_adaptive_threshold_constant_dc_becomes_zero — "
                    "x[%d] = %f, want 0\n", i, x[i]);
            assert(false);
        }
    }
    printf("PASS: test_adaptive_threshold_constant_dc_becomes_zero\n");
}

// Negative inputs must be clamped to zero. This double-tests two things:
// (a) the post-subtraction clamp is present; (b) the positive-only mean
// behaves as documented (negatives contribute zero to the sum, so a
// negative-only input has mean 0, x[i] - 0 = x[i] < 0, clamp gives 0).
void test_adaptive_threshold_negative_input_clamped_to_zero() {
    constexpr int N = 32;
    float x[N], scratch[N];
    for (int i = 0; i < N; i++) x[i] = -1.0f;
    TempoEstimator::adaptive_threshold(x, N, scratch);
    for (int i = 0; i < N; i++) {
        if (x[i] != 0.0f) {
            fprintf(stderr,
                    "FAIL: test_adaptive_threshold_negative_input_clamped_to_zero "
                    "— x[%d] = %f, want 0\n", i, x[i]);
            assert(false);
        }
    }
    printf("PASS: test_adaptive_threshold_negative_input_clamped_to_zero\n");
}

// A single spike on a zero baseline should be preserved as a peak. This is
// the load-bearing property the reference uses adaptive_threshold for: peaks
// in the onset DF stand out after the local mean is subtracted.
void test_adaptive_threshold_preserves_spike_above_baseline() {
    constexpr int N = 32;
    float x[N], scratch[N];
    for (int i = 0; i < N; i++) x[i] = 0.0f;
    constexpr int kSpike = 20;
    constexpr float kSpikeAmp = 10.0f;
    x[kSpike] = kSpikeAmp;
    TempoEstimator::adaptive_threshold(x, N, scratch);

    // Spike position should still be positive (peak preserved). Off-peak
    // positions should be zero (clamped after subtracting positive mean).
    if (!(x[kSpike] > 0.5f * kSpikeAmp)) {
        fprintf(stderr,
                "FAIL: spike at i=%d expected to remain > 5.0, got %f\n",
                kSpike, x[kSpike]);
        assert(false);
    }
    for (int i = 0; i < N; i++) {
        if (i == kSpike) continue;
        if (x[i] != 0.0f) {
            fprintf(stderr,
                    "FAIL: off-spike position x[%d] expected 0, got %f\n",
                    i, x[i]);
            assert(false);
        }
    }
    printf("PASS: test_adaptive_threshold_preserves_spike_above_baseline "
           "(spike=%.3f)\n", x[kSpike]);
}

void test_balanced_acf_pulse_train_peaks_uniform() {
    constexpr int N = 32;
    constexpr int T = 8;
    float x[N] = {0};
    float acf[N] = {0};
    for (int i = 0; i < N; i += T) x[i] = 1.0f;  // 4 pulses

    TempoEstimator::balanced_acf(x, N, acf);

    constexpr float kExpected = 0.125f;
    constexpr float kTol = 1e-5f;
    for (int lag = 0; lag < N; lag += T) {
        if (fabsf(acf[lag] - kExpected) > kTol) {
            fprintf(stderr,
                    "FAIL: balanced_acf at multiple-of-period lag — "
                    "acf[%d] = %f, want %f\n", lag, acf[lag], kExpected);
            assert(false);
        }
    }
    for (int lag = 0; lag < N; lag++) {
        if (lag % T == 0) continue;
        if (fabsf(acf[lag]) > kTol) {
            fprintf(stderr,
                    "FAIL: balanced_acf at non-period lag — "
                    "acf[%d] = %f, want 0\n", lag, acf[lag]);
            assert(false);
        }
    }
    printf("PASS: test_balanced_acf_pulse_train_peaks_uniform "
           "(acf[0]=%f, acf[8]=%f, acf[16]=%f, acf[24]=%f)\n",
           acf[0], acf[T], acf[2*T], acf[3*T]);
}

void test_acf_interp_exact_and_fractional() {
    float acf[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    // Exact indices return the sample.
    assert(std::fabs(TempoEstimator::acf_interp(acf, 8, 3.0f) - 3.0f) < 1e-6f);
    // Fractional lag interpolates linearly.
    assert(std::fabs(TempoEstimator::acf_interp(acf, 8, 3.25f) - 3.25f) < 1e-6f);
    // Out of range clamps to zero.
    assert(TempoEstimator::acf_interp(acf, 8, -0.5f) == 0.0f);
    assert(TempoEstimator::acf_interp(acf, 8, 7.5f) == 0.0f);
    printf("PASS: test_acf_interp_exact_and_fractional\n");
}

void test_parabolic_offset_centers_peak() {
    // Symmetric neighbours -> offset 0.
    assert(std::fabs(TempoEstimator::parabolic_offset(1.0f, 2.0f, 1.0f)) < 1e-6f);
    // Peak leaning right -> positive offset, bounded by 0.5.
    float off = TempoEstimator::parabolic_offset(1.0f, 2.0f, 1.9f);
    assert(off > 0.0f && off <= 0.5f);
    // Degenerate flat input -> 0 (no NaN).
    assert(TempoEstimator::parabolic_offset(2.0f, 2.0f, 2.0f) == 0.0f);
    printf("PASS: test_parabolic_offset_centers_peak\n");
}

// Build a synthetic ACF with Gaussian peaks (sigma ~1.2 frames) at the
// given lags/heights, zero elsewhere.
static void make_acf_with_peaks(float *acf, int len, const float *lags,
                                const float *heights, int n_peaks) {
    for (int i = 0; i < len; i++) acf[i] = 0.0f;
    for (int p = 0; p < n_peaks; p++) {
        for (int i = 0; i < len; i++) {
            float d = static_cast<float>(i) - lags[p];
            acf[i] += heights[p] * std::exp(-(d * d) / (2.0f * 1.2f * 1.2f));
        }
    }
}

void test_harmonic_score_peaks_at_true_tempo() {
    // ACF of a 120.2 BPM pulse train: peaks at 43, 86, 129, 172.
    float acf[512];
    const float lags[4] = {43.0f, 86.0f, 129.0f, 172.0f};
    const float heights[4] = {1.0f, 0.8f, 0.6f, 0.5f};
    make_acf_with_peaks(acf, 512, lags, heights, 4);
    float best_bpm = 0.0f, best = -1.0f;
    for (int b = 60; b <= 180; b++) {
        float s = TempoEstimator::harmonic_score_at(acf, 512, (float)b);
        if (s > best) { best = s; best_bpm = (float)b; }
    }
    // 86.13 * 60 / 43 = 120.2
    assert(std::fabs(best_bpm - 120.0f) <= 1.0f);
    printf("PASS: test_harmonic_score_peaks_at_true_tempo\n");
}

void test_no_4_3_alias_from_bar_harmonic() {
    // ACF of a ~152 BPM beat (lag 34): peaks at 34, 68, 102, 136. The old
    // comb gave the 114 BPM candidate (lag 45.3) nearly-tied mass because
    // its a=3 integer window caught the bar harmonic at lag 136.
    float acf[512];
    const float lags[4] = {34.0f, 68.0f, 102.0f, 136.0f};
    const float heights[4] = {1.0f, 0.8f, 0.6f, 0.5f};
    make_acf_with_peaks(acf, 512, lags, heights, 4);
    const float s_true  = TempoEstimator::harmonic_score_at(acf, 512, 152.0f);
    const float s_alias = TempoEstimator::harmonic_score_at(acf, 512, 114.0f);
    // The true tempo must dominate the 4:3 alias by a wide margin.
    assert(s_true > 3.0f * s_alias);
    printf("PASS: test_no_4_3_alias_from_bar_harmonic (true=%.3f alias=%.3f)\n",
           s_true, s_alias);
}

int main() {
    test_grid_constants_consistent();
    test_adaptive_threshold_constant_dc_becomes_zero();
    test_adaptive_threshold_negative_input_clamped_to_zero();
    test_adaptive_threshold_preserves_spike_above_baseline();
    test_balanced_acf_pulse_train_peaks_uniform();
    test_acf_interp_exact_and_fractional();
    test_parabolic_offset_centers_peak();
    test_harmonic_score_peaks_at_true_tempo();
    test_no_4_3_alias_from_bar_harmonic();
    printf("ALL TEMPO ESTIMATOR TESTS PASSED\n");
    return 0;
}
