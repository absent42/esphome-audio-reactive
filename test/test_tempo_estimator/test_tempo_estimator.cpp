// Unit tests for TempoEstimator. Native build:
//   g++ -std=c++17 -O2 -I components/audio_reactive \
//       test/test_tempo_estimator/test_tempo_estimator.cpp \
//       -o /tmp/test_tempo_estimator && /tmp/test_tempo_estimator
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "tempo_estimator.h"
#include "../test_helpers_rhythm.h"

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

void test_tempo_prior_gentle_and_centered() {
    // Peak at 120, symmetric in log-space, gentle at the edges.
    assert(std::fabs(TempoEstimator::tempo_prior(120.0f) - 1.0f) < 1e-4f);
    float p60 = TempoEstimator::tempo_prior(60.0f);
    float p240_equiv = TempoEstimator::tempo_prior(240.0f);
    assert(std::fabs(p60 - p240_equiv) < 1e-4f);  // one octave either side
    assert(p60 > 0.55f);                          // gentle: no hard attractor
    assert(TempoEstimator::tempo_prior(150.0f) >
           TempoEstimator::tempo_prior(180.0f));
    printf("PASS: test_tempo_prior_gentle_and_centered\n");
}

void test_peak_mass_fraction_flat_vs_spiked() {
    // Flat vector: a +-2 window holds 5/121 of the mass -> < 0.05.
    float flat[121];
    for (int i = 0; i < 121; i++) flat[i] = 1.0f;
    float c_flat = TempoEstimator::peak_mass_fraction(flat, 121, 60, 2);
    assert(std::fabs(c_flat - 5.0f / 121.0f) < 1e-4f);
    assert(c_flat < 0.05f);
    // Single spike: all mass inside the window -> 1.0.
    float spiked[121] = {};
    spiked[60] = 1.0f;
    assert(TempoEstimator::peak_mass_fraction(spiked, 121, 60, 2) == 1.0f);
    // Spike at the edge: window clamps, still 1.0.
    float edge[121] = {};
    edge[0] = 1.0f;
    assert(TempoEstimator::peak_mass_fraction(edge, 121, 0, 2) == 1.0f);
    // All-zero input must not divide by zero.
    float zeros[121] = {};
    assert(TempoEstimator::peak_mass_fraction(zeros, 121, 60, 2) == 0.0f);
    printf("PASS: test_peak_mass_fraction_flat_vs_spiked\n");
}

// Feed observe() n_updates windows containing a clean pulse train at `bpm`.
// Returns the last estimate.
static TempoEstimator::Estimate feed_pulse_windows(TempoEstimator &te,
                                                   float bpm, int n_updates) {
    float win[TempoEstimator::kWindowLen];
    const float period = TempoEstimator::kFrameHz * 60.0f / bpm;
    TempoEstimator::Estimate est{};
    for (int u = 0; u < n_updates; u++) {
        for (int i = 0; i < TempoEstimator::kWindowLen; i++) win[i] = 0.0f;
        // Phase-shift each window so it looks like a sliding stream.
        float t = std::fmod(static_cast<float>(u) * 37.0f, period);
        while (t < TempoEstimator::kWindowLen) {
            win[static_cast<int>(t)] = 10.0f;
            t += period;
        }
        est = te.observe(win, TempoEstimator::kWindowLen);
    }
    return est;
}

void test_observe_locks_on_consistent_evidence() {
    TempoEstimator te;
    auto est = feed_pulse_windows(te, 152.0f, 8);
    assert(est.locked);
    assert(std::fabs(est.bpm - 152.0f) <= 2.0f);
    assert(est.confidence > 0.3f);
    printf("PASS: test_observe_locks_on_consistent_evidence (bpm=%.1f conf=%.2f)\n",
           est.bpm, est.confidence);
}

void test_observe_escapes_stale_lock_within_bounded_updates() {
    TempoEstimator te;
    feed_pulse_windows(te, 150.0f, 20);          // strong stale lock
    // New song: 100 BPM. Must relock within 12 updates (~6-8 s of music).
    auto est = feed_pulse_windows(te, 100.0f, 12);
    assert(std::fabs(est.bpm - 100.0f) <= 2.0f);
    assert(est.locked);
    printf("PASS: test_observe_escapes_stale_lock_within_bounded_updates\n");
}

void test_observe_zero_window_zeroes_confidence() {
    TempoEstimator te;
    feed_pulse_windows(te, 120.0f, 8);
    float zeros[TempoEstimator::kWindowLen] = {};
    // A zero window carries no evidence: confidence drops to 0 immediately
    // (there is no EMA decay path any more).
    auto est = te.observe(zeros, TempoEstimator::kWindowLen);
    assert(est.confidence == 0.0f);
    for (int u = 0; u < 5; u++) est = te.observe(zeros, TempoEstimator::kWindowLen);
    assert(est.confidence == 0.0f);
    printf("PASS: test_observe_zero_window_zeroes_confidence\n");
}

void test_observe_recovers_from_nan_window() {
    // One poisoned window (e.g. a NaN escaping the FFT path) must not
    // permanently corrupt state_: without the !(x > eps) silence guard the
    // NaN spreads through state_ and the argmax pins at index 0 (60 BPM)
    // with confidence 1.0 forever.
    TempoEstimator te;
    feed_pulse_windows(te, 128.0f, 8);
    float win[TempoEstimator::kWindowLen];
    for (int i = 0; i < TempoEstimator::kWindowLen; i++) win[i] = 0.0f;
    win[TempoEstimator::kWindowLen / 2] = std::nanf("");
    te.observe(win, TempoEstimator::kWindowLen);
    auto est = feed_pulse_windows(te, 128.0f, 8);
    assert(std::isfinite(est.bpm));
    assert(std::fabs(est.bpm - 128.0f) <= 2.0f);
    printf("PASS: test_observe_recovers_from_nan_window (bpm=%.2f)\n", est.bpm);
}

void test_observe_sub_grid_resolution() {
    // 115 and 116 BPM must produce distinct estimates (old code: both -> 114).
    TempoEstimator te1, te2;
    auto e1 = feed_pulse_windows(te1, 115.0f, 10);
    auto e2 = feed_pulse_windows(te2, 116.0f, 10);
    assert(std::fabs(e1.bpm - 115.0f) <= 1.5f);
    assert(std::fabs(e2.bpm - 116.0f) <= 1.5f);
    printf("PASS: test_observe_sub_grid_resolution (%.2f, %.2f)\n", e1.bpm, e2.bpm);
}

// Slide a kWindowLen window through a long club envelope, calling observe()
// every `hop_frames` (simulating per-beat cadence).
static TempoEstimator::Estimate run_club(float bpm, float seconds,
                                         uint32_t rng_seed) {
    static float env[8192];
    rhythm_fixtures::seed(rng_seed);
    int n = rhythm_fixtures::build_club(bpm, seconds, TempoEstimator::kFrameHz,
                                        env, 8192);
    TempoEstimator te;
    TempoEstimator::Estimate est{};
    const int hop = (int)(TempoEstimator::kFrameHz * 60.0f / bpm);  // per beat
    for (int start = 0; start + TempoEstimator::kWindowLen <= n; start += hop)
        est = te.observe(env + start, TempoEstimator::kWindowLen);
    return est;
}

void test_club_patterns_no_114_150_attractors() {
    struct Case { float truth; } cases[] = {{115}, {116}, {122}, {128},
                                            {152}, {153}};
    const uint32_t seeds[] = {42, 555, 90210};
    for (auto &c : cases) {
        for (uint32_t s : seeds) {
            auto est = run_club(c.truth, 30.0f, s);
            if (std::fabs(est.bpm - c.truth) > 2.0f) {
                fprintf(stderr, "FAIL: club %.0f BPM seed %u -> %.2f (want +-2)\n",
                        c.truth, s, est.bpm);
                assert(false);
            }
            // Music must be publishable: confidence comfortably above the 0.3 gate.
            if (!(est.confidence >= 0.4f)) {
                fprintf(stderr, "FAIL: club %.0f BPM seed %u conf=%.2f (want >= 0.4)\n",
                        c.truth, s, est.confidence);
                assert(false);
            }
            printf("  club %.0f seed %u -> %.2f conf=%.2f\n",
                   c.truth, s, est.bpm, est.confidence);
        }
    }
    printf("PASS: test_club_patterns_no_114_150_attractors\n");
}

// DOCUMENTED LIMITATION - sub-harmonic locks above ~160 BPM with strong
// eighth-note content. Measured on 30 s club fixtures (seeds 42/555/90210):
// 168 -> 112.0-112.1 at conf 0.33 (3:2 alias; the eighth-note hats put ACF
// peaks on the T/2 grid, the 2/3-tempo candidate's harmonics 1.5T/3T/4.5T/6T
// all land on that grid, and the 120-centred prior tips the tie), and
// 176 -> 88.0 / 117.3 at conf 0.00-0.34 (2:1 or 3:2 depending on seed). A
// half-lag template term (score += w * acf[0.5 * lag], w swept over
// [0.3, 0.8]) was tried and reverted: the HALF-tempo candidate's half-lag is
// the true beat lag T - always the strongest ACF peak - so the term flipped
// 168 -> 84 and even the eighth-free 150 metronome -> 75 at every weight.
// The alias confidence (up to 0.34) sits marginally above the 0.3 publish
// gate, so the sensor may publish the sub-harmonic for such material; this
// test pins the failure to a harmonic fraction of the truth with marginal
// confidence so any drift is caught.
void test_club_high_tempo_subharmonic_documented_limitation() {
    struct Case { float truth; } cases[] = {{168}, {176}};
    const uint32_t seeds[] = {42, 555, 90210};
    for (auto &c : cases) {
        for (uint32_t s : seeds) {
            auto est = run_club(c.truth, 30.0f, s);
            const bool correct  = std::fabs(est.bpm - c.truth) <= 2.0f;
            const bool alias_23 = std::fabs(est.bpm - c.truth * (2.0f / 3.0f)) <= 2.5f;
            const bool alias_12 = std::fabs(est.bpm - c.truth * 0.5f) <= 2.5f;
            if (!(correct || alias_23 || alias_12)) {
                fprintf(stderr,
                        "FAIL: club %.0f seed %u -> %.2f (neither truth nor a "
                        "documented 2:3 / 1:2 sub-harmonic)\n",
                        c.truth, s, est.bpm);
                assert(false);
            }
            // When aliased, confidence must stay marginal (measured <= 0.34).
            if (!correct && !(est.confidence <= 0.40f)) {
                fprintf(stderr, "FAIL: club %.0f seed %u aliased to %.2f with "
                        "conf %.2f (> 0.40)\n", c.truth, s, est.bpm, est.confidence);
                assert(false);
            }
            printf("  club %.0f seed %u -> %.2f conf=%.2f (%s)\n",
                   c.truth, s, est.bpm, est.confidence,
                   correct ? "correct" : "documented sub-harmonic");
        }
    }
    printf("PASS: test_club_high_tempo_subharmonic_documented_limitation\n");
}

void test_speech_noise_stays_unconfident() {
    static float env[8192];
    rhythm_fixtures::seed(555);
    int n = rhythm_fixtures::build_speech(60.0f, TempoEstimator::kFrameHz, env, 8192);
    TempoEstimator te;
    int confident = 0, total = 0;
    for (int start = 0; start + TempoEstimator::kWindowLen <= n; start += 43) {
        auto est = te.observe(env + start, TempoEstimator::kWindowLen);
        total++;
        if (est.confidence >= 0.3f) confident++;
    }
    // At most 10% of updates may cross the publish gate on non-music.
    if (confident * 10 > total) {
        fprintf(stderr, "FAIL: speech noise confident on %d/%d updates\n",
                confident, total);
        assert(false);
    }
    printf("PASS: test_speech_noise_stays_unconfident (%d/%d)\n", confident, total);
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
    test_tempo_prior_gentle_and_centered();
    test_peak_mass_fraction_flat_vs_spiked();
    test_observe_locks_on_consistent_evidence();
    test_observe_escapes_stale_lock_within_bounded_updates();
    test_observe_zero_window_zeroes_confidence();
    test_observe_recovers_from_nan_window();
    test_observe_sub_grid_resolution();
    test_club_patterns_no_114_150_attractors();
    test_club_high_tempo_subharmonic_documented_limitation();
    test_speech_noise_stays_unconfident();
    printf("ALL TEMPO ESTIMATOR TESTS PASSED\n");
    return 0;
}
