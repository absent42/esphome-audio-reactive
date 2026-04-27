// test/test_btrack/test_btrack.cpp
//
// Unit tests for the BTrack rewrite (see docs analysis report — re-implementation
// is happening step-by-step with TDD). Tests are added alongside each step's
// implementation; end-to-end metronome / real-music tests live in their own
// test directories (added at steps 7-8).
//
// Build (native):  g++ -std=c++17 -O2 -I.. test_btrack.cpp -o test_btrack
//                  (PlatformIO `pio test -e native -f test_btrack` does this in CI)

#include <cassert>
#include <cmath>
#include <cstdio>

#define AUDIO_REACTIVE_NATIVE_TEST
#include "../../components/audio_reactive/btrack.h"

using namespace esphome::audio_reactive;

// ---------------------------------------------------------------------------
// Step 1 — adaptive_threshold
// ---------------------------------------------------------------------------
// The reference (adamstark BTrack, calculateTempo() in src/BTrack.cpp) calls
// adaptiveThreshold() on the onset DF before computing the autocorrelation
// function. Without this step the always-positive SuperFlux baseline acts as
// a DC bias that dominates the ACF and buries beat-period peaks.
//
// Reference signature (1-based-aware):  void adaptiveThreshold(double *x, int N)
//   - subtracts a local moving-mean computed in [i - 8, i + 7] (clipped to
//     [0, N-1] at edges), then clamps the result at zero.
//   - the "mean" is BTrack's mean_array() — sums positive values only, divides
//     by the full window length. Negative samples contribute zero to the sum.
//
// The DC-removal property is the most load-bearing behavioural guarantee
// (a constant input must produce all-zero output, because the local mean
// equals the input). The first test exercises that.

void test_adaptive_threshold_constant_dc_becomes_zero() {
    constexpr int N = 32;
    float x[N], scratch[N];
    for (int i = 0; i < N; i++) x[i] = 5.0f;
    BTrack::adaptive_threshold(x, N, scratch);
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
    BTrack::adaptive_threshold(x, N, scratch);
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
    BTrack::adaptive_threshold(x, N, scratch);

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

// ---------------------------------------------------------------------------
// Step 2 — balanced_acf
// ---------------------------------------------------------------------------
// The reference's tempo-induction pipeline runs balanced_acf on the (already-
// adaptive-thresholded) onset DF. The "balanced" property is the load-bearing
// one: by dividing the sum at lag k by (N - k) (the count of valid pairs at
// that lag), peaks at periodic structure stand at the same height regardless
// of which multiple of the period they're at.
//
// Test signal: pulse train of N=32 samples with pulses at indices 0, 8, 16,
// 24 (period T=8, four pulses, all unit amplitude). Theoretical balanced ACF:
//
//   acf[0]  = 4   pairs / (32 -  0) = 4/32 = 0.125    // self-correlation
//   acf[8]  = 3   pairs / (32 -  8) = 3/24 = 0.125    // pulse 0↔1, 1↔2, 2↔3
//   acf[16] = 2   pairs / (32 - 16) = 2/16 = 0.125    // pulse 0↔2, 1↔3
//   acf[24] = 1   pair  / (32 - 24) = 1/ 8 = 0.125    // pulse 0↔3
//   acf[i]  = 0  for i not a multiple of 8
//
// All four peaks at the same height (0.125) is the balanced property — an
// UN-balanced ACF (sum / N) would taper from 0.125 → 0.094 → 0.063 → 0.031
// at increasing lag multiples, which would make harmonic-summing in the
// comb filterbank biased toward the lowest harmonic.

void test_balanced_acf_pulse_train_peaks_uniform() {
    constexpr int N = 32;
    constexpr int T = 8;
    float x[N] = {0};
    float acf[N] = {0};
    for (int i = 0; i < N; i += T) x[i] = 1.0f;  // 4 pulses

    BTrack::balanced_acf(x, N, acf);

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

// ---------------------------------------------------------------------------
// Step 3 — rayleigh_weights + comb_filterbank
// ---------------------------------------------------------------------------
// Rayleigh distribution w[n] = (n / r²) · exp(-n² / 2r²) has a single mode
// at n = r and is zero at n=0. Sanity-check those two anchors.

void test_rayleigh_weights_peak_at_r() {
    constexpr int N = 128;
    constexpr float r = 43.0f;
    float w[N];
    BTrack::rayleigh_weights(r, N, w);

    // w[0] is exactly zero (n factor in the formula).
    if (w[0] != 0.0f) {
        fprintf(stderr,
                "FAIL: rayleigh_weights w[0] = %f, want 0\n", w[0]);
        assert(false);
    }
    // argmax should be at n = round(r) = 43.
    int argmax = 0;
    float maxv = w[0];
    for (int i = 1; i < N; i++) {
        if (w[i] > maxv) { maxv = w[i]; argmax = i; }
    }
    if (argmax != static_cast<int>(r)) {
        fprintf(stderr,
                "FAIL: rayleigh_weights argmax = %d, want %d (peak value %f)\n",
                argmax, static_cast<int>(r), maxv);
        assert(false);
    }
    printf("PASS: test_rayleigh_weights_peak_at_r "
           "(argmax=%d, w[r]=%f)\n", argmax, maxv);
}

// Comb filterbank with a single ACF peak at lag k: argmax of comb_fb output
// should be at i = k (the period candidate aligned with that lag, via the
// a=1 path which reads acf[i+a-1+b] with a=1, b=0 → acf[i]).
//
// We pick k=43 (the period at 120 BPM, where the Rayleigh prior also peaks)
// so the argmax-at-k property doesn't depend on the prior — but a follow-up
// test below picks a non-prior-aligned k to prove the comb FB tracks the
// ACF peak rather than simply locking on the prior.

void test_comb_filterbank_argmax_follows_acf_peak_at_prior() {
    constexpr int kAcfLen = 256;
    constexpr int kNPer = 128;
    constexpr int kPeak = 43;

    float acf[kAcfLen] = {0};
    acf[kPeak] = 1.0f;

    float weights[kNPer];
    BTrack::rayleigh_weights(43.0f, kNPer, weights);

    float comb_fb[kNPer] = {0};
    BTrack::comb_filterbank(acf, kAcfLen, weights, kNPer, comb_fb);

    int argmax = 0;
    float maxv = comb_fb[0];
    for (int i = 1; i < kNPer; i++) {
        if (comb_fb[i] > maxv) { maxv = comb_fb[i]; argmax = i; }
    }
    if (argmax != kPeak) {
        fprintf(stderr,
                "FAIL: comb_filterbank argmax = %d, want %d\n",
                argmax, kPeak);
        assert(false);
    }
    printf("PASS: test_comb_filterbank_argmax_follows_acf_peak_at_prior "
           "(argmax=%d, comb_fb[argmax]=%f)\n", argmax, maxv);
}

// Critical: prove the comb FB does NOT lock onto the prior peak when the
// ACF says otherwise. ACF peak at lag=60 (≈86 BPM, well away from the 120-BPM
// Rayleigh prior) → comb FB argmax must be at 60, not at 43. This is the
// property the previous port broke (its biased Viterbi initial prior pulled
// every lock toward 120 BPM regardless of music tempo).

void test_comb_filterbank_argmax_follows_acf_peak_off_prior() {
    constexpr int kAcfLen = 256;
    constexpr int kNPer = 128;
    constexpr int kPeak = 60;  // ≈86 BPM at 86.13 fps frame rate

    float acf[kAcfLen] = {0};
    acf[kPeak] = 1.0f;

    float weights[kNPer];
    BTrack::rayleigh_weights(43.0f, kNPer, weights);

    float comb_fb[kNPer] = {0};
    BTrack::comb_filterbank(acf, kAcfLen, weights, kNPer, comb_fb);

    int argmax = 0;
    float maxv = comb_fb[0];
    for (int i = 1; i < kNPer; i++) {
        if (comb_fb[i] > maxv) { maxv = comb_fb[i]; argmax = i; }
    }
    if (argmax != kPeak) {
        fprintf(stderr,
                "FAIL: off-prior comb_filterbank argmax = %d, want %d "
                "(comb_fb[43]=%f, comb_fb[%d]=%f)\n",
                argmax, kPeak, comb_fb[43], kPeak, comb_fb[kPeak]);
        assert(false);
    }
    printf("PASS: test_comb_filterbank_argmax_follows_acf_peak_off_prior "
           "(argmax=%d, comb_fb[argmax]=%f, comb_fb[43]=%f)\n",
           argmax, maxv, comb_fb[43]);
}

// ---------------------------------------------------------------------------
// Step 4 — Viterbi tempo update
// ---------------------------------------------------------------------------
// The 41-candidate grid maps i → BPM = 80 + 2i. So argmax(delta) = i means
// "current tempo estimate = 80 + 2i BPM". The reference's initial prevDelta
// is uniform 1.0 — the property that broke in our previous port (which used
// a tightly-peaked Gaussian centred on i=20 = 120 BPM and so couldn't lock
// off-prior tempos).

void test_build_tempo_observation_picks_correct_candidate_for_120bpm() {
    constexpr int kNPer = 128;
    constexpr int kNCand = BTrack::kTempoCandidates;
    // 120 BPM → lag = round(5168/120) = 43. comb_fb index = 43-1 = 42 (the
    // build_tempo_observation reads comb_fb[lag - 1]).
    float comb_fb[kNPer] = {0};
    comb_fb[42] = 1.0f;

    float obs[kNCand] = {0};
    BTrack::build_tempo_observation(comb_fb, kNPer, obs, kNCand);

    // Expect the largest entry at i=20 (BPM=120). Other candidates may have
    // small contributions if their lag2 (double-tempo) sample falls on the
    // peak — but the i=20 entry should dominate since both lag1 and lag2
    // sample non-peak indices for it (lag2 for i=20 = round(5168/240) = 22,
    // comb_fb[21] = 0).
    int argmax = 0;
    for (int i = 1; i < kNCand; i++) if (obs[i] > obs[argmax]) argmax = i;
    if (argmax != 20) {
        fprintf(stderr,
                "FAIL: build_tempo_observation argmax = %d, want 20 "
                "(obs[20]=%f, obs[argmax]=%f)\n",
                argmax, obs[20], obs[argmax]);
        assert(false);
    }
    printf("PASS: test_build_tempo_observation_picks_correct_candidate_for_120bpm "
           "(obs[20]=%f)\n", obs[20]);
}

void test_tempo_transition_matrix_diagonal_is_max() {
    constexpr int kNCand = BTrack::kTempoCandidates;
    float tm[kNCand * kNCand];
    BTrack::build_tempo_transition_matrix(BTrack::kTempoSigma, kNCand, tm);

    // Each row's argmax must be on the diagonal (transition Gaussian centred
    // at i; prob of staying at i is highest).
    for (int i = 0; i < kNCand; i++) {
        int argmax = 0;
        float maxv = tm[i * kNCand + 0];
        for (int j = 1; j < kNCand; j++) {
            if (tm[i * kNCand + j] > maxv) {
                maxv = tm[i * kNCand + j];
                argmax = j;
            }
        }
        if (argmax != i) {
            fprintf(stderr,
                    "FAIL: tm row %d argmax = %d, want %d (max=%f)\n",
                    i, argmax, i, maxv);
            assert(false);
        }
        // Diagonal should be exactly 1.0 (exp(0)).
        if (fabsf(tm[i * kNCand + i] - 1.0f) > 1e-5f) {
            fprintf(stderr,
                    "FAIL: tm[%d][%d] = %f, want 1.0\n",
                    i, i, tm[i * kNCand + i]);
            assert(false);
        }
    }
    printf("PASS: test_tempo_transition_matrix_diagonal_is_max\n");
}

// CRITICAL: with a uniform initial prevDelta and a comb_fb peaked at the
// 90 BPM lag (off the 120 BPM Rayleigh prior), the Viterbi step must lock
// onto candidate i=5 (= (90-80)/2). This is the key property the previous
// port broke: its biased initial delta pulled lock toward i=20 regardless
// of observation.

void test_viterbi_step_locks_off_prior_with_uniform_initial_delta() {
    constexpr int kNPer = 128;
    constexpr int kNCand = BTrack::kTempoCandidates;
    constexpr int kTargetCand = 5;          // BPM = 80 + 2*5 = 90
    // 90 BPM → lag = round(5168/90) = 57. comb_fb index = 57-1 = 56.
    float comb_fb[kNPer] = {0};
    comb_fb[56] = 1.0f;

    float obs[kNCand] = {0};
    BTrack::build_tempo_observation(comb_fb, kNPer, obs, kNCand);

    float tm[kNCand * kNCand];
    BTrack::build_tempo_transition_matrix(BTrack::kTempoSigma, kNCand, tm);

    float prev_delta[kNCand];
    for (int i = 0; i < kNCand; i++) prev_delta[i] = 1.0f;  // uniform

    float delta[kNCand];
    BTrack::viterbi_step(prev_delta, obs, tm, kNCand, delta);

    int argmax = 0;
    for (int i = 1; i < kNCand; i++) if (delta[i] > delta[argmax]) argmax = i;
    if (argmax != kTargetCand) {
        fprintf(stderr,
                "FAIL: viterbi_step argmax = %d, want %d "
                "(delta[5]=%f, delta[20]=%f)\n",
                argmax, kTargetCand, delta[5], delta[20]);
        assert(false);
    }
    // Output should be normalised (sum to 1 with floats — tolerate eps).
    float sum = 0.0f;
    for (int i = 0; i < kNCand; i++) sum += delta[i];
    if (fabsf(sum - 1.0f) > 1e-4f) {
        fprintf(stderr,
                "FAIL: viterbi_step output not normalised (sum = %f)\n", sum);
        assert(false);
    }
    printf("PASS: test_viterbi_step_locks_off_prior_with_uniform_initial_delta "
           "(argmax=%d → BPM=%d, sum=%f)\n",
           argmax, 80 + 2 * argmax, sum);
}

// ---------------------------------------------------------------------------
// Step 5 — log-Gaussian weights / cumulative score DP / predict_beat
// ---------------------------------------------------------------------------
// log_gaussian_weights() builds the past-window weighting used in both the
// cumulative-score DP step and predict_beat. With v starting at -2*beat_period
// and incrementing by 1 each sample, the weight peaks (= 1) at v = -beat_period,
// i.e. at index = beat_period. Verify peak location and value.

void test_log_gaussian_weights_peak_at_beat_period() {
    constexpr int N = 30;
    constexpr float kBeatPeriod = 10.0f;
    constexpr float kTightness = BTrack::kTightness;
    float w[N];
    BTrack::log_gaussian_weights(kBeatPeriod, kTightness, N, w);

    // Peak should be exactly 1.0 at the expected index.
    const int peak_idx = static_cast<int>(kBeatPeriod);
    if (fabsf(w[peak_idx] - 1.0f) > 1e-5f) {
        fprintf(stderr,
                "FAIL: log_gaussian_weights w[%d] = %f, want 1.0\n",
                peak_idx, w[peak_idx]);
        assert(false);
    }
    // The peak must be a global max.
    for (int i = 0; i < N; i++) {
        if (i != peak_idx && w[i] >= w[peak_idx]) {
            fprintf(stderr,
                    "FAIL: w[%d] = %f >= w[%d] = %f (peak should be unique)\n",
                    i, w[i], peak_idx, w[peak_idx]);
            assert(false);
        }
    }
    printf("PASS: test_log_gaussian_weights_peak_at_beat_period "
           "(w[10]=%f, w[5]=%f, w[20]=%f)\n", w[10], w[5], w[20]);
}

// Integration: with beat_period fixed at 43 (120 BPM at 86 fps) and a perfect
// pulse-train onset stream at the same period, the cumulative-score DP plus
// predict_beat should produce beat events at intervals ≈ 43 frames.
//
// Tempo induction (Viterbi) is not exercised here — we override beat_period
// via debug_set_beat_period_frames() so the DP path runs in isolation. Step 6
// will rerun a similar test without the override and prove the Viterbi finds
// the period itself.

void test_periodic_input_produces_beats_at_expected_cadence() {
    BTrack bt;
    bt.reset();
    constexpr float kBp = 43.0f;
    bt.debug_set_beat_period_frames(kBp);

    // Onset pulse train at exactly the beat period.
    // Run for 10 seconds (≈860 frames). Track beat-event frames.
    constexpr int kFrames = static_cast<int>(BTrack::kFrameHz * 10.0f);
    int events[64] = {0};
    int n_events = 0;
    int next_pulse = 0;
    for (int f = 0; f < kFrames; f++) {
        const float onset = (f == next_pulse) ? 10.0f : 0.0f;
        if (f == next_pulse) next_pulse += static_cast<int>(kBp);
        // Re-assert beat period each frame so any stray Viterbi-side update
        // (none in step 5, but defensive for step 6) doesn't perturb the test.
        bt.debug_set_beat_period_frames(kBp);
        const auto r = bt.process(onset);
        if (r.beat_event && n_events < 64) events[n_events++] = f;
    }

    // Skip the first few events (algorithm settling). Use only events from
    // the second half of the run for cadence measurement.
    if (n_events < 6) {
        fprintf(stderr,
                "FAIL: expected at least 6 beat events in %d frames at 120 BPM, "
                "got %d\n", kFrames, n_events);
        assert(false);
    }
    int sum = 0, count = 0;
    for (int i = n_events / 2 + 1; i < n_events; i++) {
        sum += (events[i] - events[i - 1]);
        count++;
    }
    if (count == 0) {
        fprintf(stderr, "FAIL: could not measure inter-beat intervals\n");
        assert(false);
    }
    const float avg_interval = static_cast<float>(sum) / static_cast<float>(count);
    if (fabsf(avg_interval - kBp) > 3.0f) {
        fprintf(stderr,
                "FAIL: avg inter-beat interval = %.2f frames, want %.2f (±3)\n",
                avg_interval, kBp);
        // Diagnostic dump of all event positions.
        fprintf(stderr, "  events:");
        for (int i = 0; i < n_events; i++) fprintf(stderr, " %d", events[i]);
        fprintf(stderr, "\n");
        assert(false);
    }
    printf("PASS: test_periodic_input_produces_beats_at_expected_cadence "
           "(n_events=%d, avg_interval=%.2f frames, want %.2f)\n",
           n_events, avg_interval, kBp);
}

// ---------------------------------------------------------------------------
// Step 6 — full pipeline integration (Viterbi tempo induction wired in).
// ---------------------------------------------------------------------------
// With the tempo path active, BTrack must derive the beat period from the
// input rather than from debug_set_beat_period_frames(). Tests at three BPMs
// — 90, 120, 150 — to confirm the lock isn't biased toward the 120 BPM
// Rayleigh prior.

static float feed_metronome_get_final_bpm(float bpm, float seconds) {
    BTrack bt;
    bt.reset();
    const float period = BTrack::kFrameHz * 60.0f / bpm;
    const int total_frames = static_cast<int>(BTrack::kFrameHz * seconds);
    int next_pulse = static_cast<int>(period);
    BTrack::Result last;
    for (int f = 0; f < total_frames; f++) {
        const float onset = (f == next_pulse) ? 10.0f : 0.0f;
        if (f == next_pulse) next_pulse += static_cast<int>(period);
        last = bt.process(onset);
    }
    return last.bpm;
}

void test_locks_on_120_bpm_via_full_pipeline() {
    const float bpm = feed_metronome_get_final_bpm(120.0f, 10.0f);
    if (fabsf(bpm - 120.0f) > 4.0f) {
        fprintf(stderr,
                "FAIL: 120 BPM pulse train → BTrack reports %.1f BPM (want 120 ±4)\n",
                bpm);
        assert(false);
    }
    printf("PASS: test_locks_on_120_bpm_via_full_pipeline (bpm=%.1f)\n", bpm);
}

void test_locks_on_90_bpm_via_full_pipeline() {
    // 90 BPM is well off the Rayleigh prior peak (43 frames ≈ 120 BPM). The
    // previous port locked at 114-120 BPM regardless of input on real music;
    // a working pipeline must lock at 90 ±4 here.
    const float bpm = feed_metronome_get_final_bpm(90.0f, 15.0f);
    if (fabsf(bpm - 90.0f) > 4.0f) {
        fprintf(stderr,
                "FAIL: 90 BPM pulse train → BTrack reports %.1f BPM (want 90 ±4)\n",
                bpm);
        assert(false);
    }
    printf("PASS: test_locks_on_90_bpm_via_full_pipeline (bpm=%.1f)\n", bpm);
}

void test_locks_on_150_bpm_via_full_pipeline() {
    const float bpm = feed_metronome_get_final_bpm(150.0f, 10.0f);
    if (fabsf(bpm - 150.0f) > 4.0f) {
        fprintf(stderr,
                "FAIL: 150 BPM pulse train → BTrack reports %.1f BPM (want 150 ±4)\n",
                bpm);
        assert(false);
    }
    printf("PASS: test_locks_on_150_bpm_via_full_pipeline (bpm=%.1f)\n", bpm);
}

int main() {
    test_adaptive_threshold_constant_dc_becomes_zero();
    test_adaptive_threshold_negative_input_clamped_to_zero();
    test_adaptive_threshold_preserves_spike_above_baseline();
    test_balanced_acf_pulse_train_peaks_uniform();
    test_rayleigh_weights_peak_at_r();
    test_comb_filterbank_argmax_follows_acf_peak_at_prior();
    test_comb_filterbank_argmax_follows_acf_peak_off_prior();
    test_build_tempo_observation_picks_correct_candidate_for_120bpm();
    test_tempo_transition_matrix_diagonal_is_max();
    test_viterbi_step_locks_off_prior_with_uniform_initial_delta();
    test_log_gaussian_weights_peak_at_beat_period();
    test_periodic_input_produces_beats_at_expected_cadence();
    test_locks_on_120_bpm_via_full_pipeline();
    test_locks_on_90_bpm_via_full_pipeline();
    test_locks_on_150_bpm_via_full_pipeline();
    printf("ALL BTRACK UNIT TESTS PASSED\n");
    return 0;
}
