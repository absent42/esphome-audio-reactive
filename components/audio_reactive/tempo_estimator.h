#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

// TempoEstimator - tempo induction for BTrack, replacing the former
// ACF -> comb-filterbank -> 41-candidate Viterbi chain. Design notes:
//
//   - Candidates are evaluated on a continuous BPM grid (60..180 in 1-BPM
//     steps) with FRACTIONAL lag interpolation into the ACF. The old
//     integer-lag sampling collapsed whole tempo neighborhoods onto single
//     values (114/116 -> lag 45, 150/152/154 -> lag 34) and carried a
//     systematic -2..-6 BPM off-by-one bias.
//   - Each candidate is scored by a harmonic template (lags T, 2T, 3T, 4T
//     with decaying weights) evaluated at exact fractional lags, so a 4:3
//     alias candidate no longer inherits bar-harmonic mass through wide
//     integer comb windows.
//   - A gentle log-normal tempo prior (center 120 BPM, sigma 1 octave)
//     nudges octave choices without creating hard attractors.
//   - Smoothing is a LEAKY INTEGRATOR over normalised scores (linear,
//     bounded memory) instead of max-product Viterbi with a blended prior:
//     old evidence decays geometrically no matter how strong it was, so any
//     wrong lock is escapable within ~10 updates.
//   - Confidence is the peak-neighborhood mass fraction of the SMOOTHED
//     state vector, gated by argmax stability across consecutive updates.
//     Each window's score is normalised to sum 1 before integration, so
//     state_ is a moving average of observation mass: on music the mass
//     piles up at the true tempo (fraction >> the 5/121 flat floor), while
//     on non-rhythmic input the random per-window peaks average out and no
//     +-2 BPM neighborhood accumulates mass -> confidence stays near 0.
//     (An earlier peak-to-median metric on the raw score failed: the raw
//     harmonic-score vector is sparse even for noise, so its median is
//     near zero and any random peak looked confident.)
//
// All heavy buffers are class members: the caller (BTrack::process on the
// FFT task) has a 6 KB stack.

namespace esphome {
namespace audio_reactive {

class TempoEstimator {
 public:
    // Frame rate of the onset stream (44100 Hz / 512 hop). Must match
    // BTrack::kFrameHz.
    static constexpr float kFrameHz = 86.13f;

    // Candidate grid: 60..180 BPM inclusive, 1-BPM steps.
    static constexpr float kBpmMin = 60.0f;
    static constexpr float kBpmMax = 180.0f;
    static constexpr int   kNumCandidates = 121;

    // Onset-window length consumed by observe(). Matches BTrack::kHistoryLen.
    static constexpr int kWindowLen = 512;

    // Harmonic template: weights for lags 1T..4T. Decaying so the
    // fundamental dominates; sub-harmonics of the true tempo credit the
    // true candidate, not its 4:3 alias.
    static constexpr int   kNumHarmonics = 4;
    static constexpr float kHarmonicWeights[kNumHarmonics] = {1.0f, 0.6f, 0.4f, 0.3f};

    // Log-normal tempo prior.
    static constexpr float kPriorCenterBpm   = 120.0f;
    static constexpr float kPriorSigmaOctaves = 1.0f;

    // Smoothing / lock behaviour.
    //   kSmoothLambda    : per-update state retention. Updates run once per
    //                      beat (~2/s at 120 BPM); 0.85^10 = 0.20, so a wrong
    //                      lock decays to noise within ~10 beats (~5 s).
    //   kStableUpdates   : consecutive agreeing updates before "locked".
    //   kStableTolBpm    : agreement tolerance between successive argmaxes.
    //   kConfHalfWidthBpm: half-width (in 1-BPM grid steps) of the peak
    //                      neighborhood whose state_ mass fraction is the
    //                      confidence.
    //   kConfScale       : mass fraction at which we call it fully
    //                      confident (monotone rescale, capped at 1).
    //                      Calibrated against the synthetic fixtures:
    //                      club patterns hold 0.16-0.20 of state_ mass in
    //                      the +-2 window once locked (harmonic-neighbor
    //                      candidates split the rest), speech noise never
    //                      exceeds 0.09. 0.35 maps those to >= 0.45 and
    //                      <= 0.25 - both clear of the 0.3 publish gate.
    static constexpr float kSmoothLambda  = 0.85f;
    static constexpr int   kStableUpdates = 4;
    static constexpr float kStableTolBpm  = 3.0f;
    static constexpr int   kConfHalfWidthBpm = 2;
    static constexpr float kConfScale = 0.35f;

    struct Estimate {
        float bpm;         // best tempo estimate (parabolically refined)
        float confidence;  // 0..1; 0 until argmax is stable
        bool  locked;      // stability gate satisfied
    };

    TempoEstimator() { reset(); }

    void reset();

    // Consume one onset-DF window (oldest sample first, length kWindowLen)
    // and produce the current tempo estimate. Called once per predicted
    // beat by BTrack. `onset_df` is not modified.
    Estimate observe(const float *onset_df, int n);

    // ------------------------------------------------------------------
    // Static helpers - public for unit tests.
    // ------------------------------------------------------------------

    // Moved verbatim from btrack.h (their unit tests move here too).
    static void adaptive_threshold(float *x, int N, float *scratch);
    static void balanced_acf(const float *in, int N, float *out);

    // Linear interpolation of acf at fractional `lag`. Returns 0 outside
    // [0, len-1].
    static float acf_interp(const float *acf, int len, float lag);

    // Harmonic-template score for one candidate BPM.
    static float harmonic_score_at(const float *acf, int len, float bpm);

    // Fraction of v's total mass inside [center-halfwidth, center+halfwidth]
    // (bounds-clamped). 0 for empty/degenerate input (also catches a NaN
    // total). Flat input -> window/n; single spike at center -> 1.
    static float peak_mass_fraction(const float *v, int n, int center,
                                    int halfwidth);

    // Log-normal prior weight at `bpm`.
    static float tempo_prior(float bpm);

    // Parabolic refinement of an argmax on a uniform grid: given values at
    // (idx-1, idx, idx+1) returns the sub-grid offset in [-0.5, 0.5].
    static float parabolic_offset(float ym1, float y0, float yp1);

 private:
    // Smoother state.
    float state_[kNumCandidates]{};
    float last_argmax_bpm_{0.0f};
    int   stable_count_{0};
    float current_bpm_{120.0f};

    // Scratch (class members - FFT task stack is 6 KB).
    float work_[kWindowLen]{};
    float thresh_scratch_[kWindowLen]{};
    float acf_[kWindowLen]{};
    float score_[kNumCandidates]{};
};

// Definition for the in-class constexpr array (required at C++14/17 for ODR
// use; harmless in C++17 single-header context).
inline constexpr float TempoEstimator::kHarmonicWeights[TempoEstimator::kNumHarmonics];

inline void TempoEstimator::reset() {
    for (int i = 0; i < kNumCandidates; i++) state_[i] = 0.0f;
    last_argmax_bpm_ = 0.0f;
    stable_count_ = 0;
    current_bpm_ = 120.0f;
}

inline void TempoEstimator::adaptive_threshold(float *x, int N, float *scratch) {
    if (N <= 0) return;
    constexpr int p_pre = 8;
    constexpr int p_post = 7;

    // Helper: BTrack's mean_array() — sums positive values in x[lo..hi]
    // (0-based, both inclusive), divides by `length` (NOT necessarily
    // hi-lo+1; matches the reference's "stated length" semantics so we
    // can faithfully replicate edge-case bias).
    auto positive_mean = [](const float *arr, int lo, int hi, int length) {
        if (length <= 0) return 0.0f;
        float sum = 0.0f;
        for (int j = lo; j <= hi; j++) {
            if (arr[j] > 0.0f) sum += arr[j];
        }
        return sum / static_cast<float>(length);
    };

    // The reference splits the buffer into three regions and uses 1-based
    // indexing internally. We reproduce the same window math in 0-based,
    // clamping to [0, N-1] to avoid the off-by-one OOB read in the
    // reference's middle loop. Comments quote the reference's 1-based form.
    const int t = std::min(N, p_post);

    // Edge 1 — i in [0, t]:  mean_array(x, 1, k)  with k = min(i+p_pre, N).
    // 0-based window: x[0 .. k-1], length = k.
    for (int i = 0; i <= t && i < N; i++) {
        const int k = std::min(i + p_pre, N);
        scratch[i] = positive_mean(x, 0, k - 1, k);
    }

    // Middle — i in [t+1, N-p_post):
    //   mean_array(x, i - p_pre, i + p_post)  → 0-based window [i-p_pre-1, i+p_post-1].
    //   The reference reads x[-1] at i=t+1=8 (undefined behaviour) and
    //   divides by a fixed length 16 even when only 15 samples are valid.
    //   That bias breaks DC removal for constant inputs at i=8 (residual
    //   ≈ 1/16 of the input). We clamp lo at 0 AND use the clamped window
    //   length as the divisor — a documented deviation from the reference's
    //   buggy edge behaviour.
    for (int i = t + 1; i < N - p_post; i++) {
        int lo = i - p_pre - 1;
        int hi = i + p_post - 1;
        if (lo < 0) lo = 0;
        if (hi >= N) hi = N - 1;
        const int length = hi - lo + 1;
        scratch[i] = positive_mean(x, lo, hi, length);
    }

    // Edge 2 — i in [N-p_post, N):  mean_array(x, k, N) with k = max(i-p_post, 1).
    // 0-based window: x[k-1 .. N-1], length = N-k+1.
    for (int i = std::max(0, N - p_post); i < N; i++) {
        const int k = std::max(i - p_post, 1);
        const int length = N - k + 1;
        scratch[i] = positive_mean(x, k - 1, N - 1, length);
    }

    for (int i = 0; i < N; i++) {
        x[i] -= scratch[i];
        if (x[i] < 0.0f) x[i] = 0.0f;
    }
}

inline void TempoEstimator::balanced_acf(const float *in, int N, float *out) {
    if (N <= 0) return;
    for (int lag = 0; lag < N; lag++) {
        const int valid_pairs = N - lag;  // ≥ 1 for lag < N
        float sum = 0.0f;
        for (int n = 0; n < valid_pairs; n++) {
            sum += in[n] * in[n + lag];
        }
        out[lag] = sum / static_cast<float>(valid_pairs);
    }
}

inline float TempoEstimator::acf_interp(const float *acf, int len, float lag) {
    if (lag < 0.0f || lag > static_cast<float>(len - 1)) return 0.0f;
    const int i = static_cast<int>(lag);
    const float frac = lag - static_cast<float>(i);
    if (i >= len - 1) return acf[len - 1];
    return acf[i] * (1.0f - frac) + acf[i + 1] * frac;
}

inline float TempoEstimator::parabolic_offset(float ym1, float y0, float yp1) {
    const float denom = ym1 - 2.0f * y0 + yp1;
    if (std::fabs(denom) < 1e-12f) return 0.0f;
    float off = 0.5f * (ym1 - yp1) / denom;
    if (off > 0.5f) off = 0.5f;
    if (off < -0.5f) off = -0.5f;
    return off;
}

inline float TempoEstimator::harmonic_score_at(const float *acf, int len, float bpm) {
    if (bpm < 1.0f) return 0.0f;
    const float lag = kFrameHz * 60.0f / bpm;
    float score = 0.0f;
    for (int h = 1; h <= kNumHarmonics; h++) {
        const float hl = lag * static_cast<float>(h);
        if (hl > static_cast<float>(len - 2)) break;  // beyond window: no credit
        score += kHarmonicWeights[h - 1] * acf_interp(acf, len, hl);
    }
    return score;
}

inline float TempoEstimator::tempo_prior(float bpm) {
    if (bpm <= 0.0f) return 0.0f;
    const float octaves = std::log2(bpm / kPriorCenterBpm);
    const float z = octaves / kPriorSigmaOctaves;
    return std::exp(-0.5f * z * z);
}

inline float TempoEstimator::peak_mass_fraction(const float *v, int n,
                                                int center, int halfwidth) {
    if (n <= 0) return 0.0f;
    float total = 0.0f;
    for (int i = 0; i < n; i++) total += v[i];
    // !(x > 0) also catches a NaN total: degenerate input -> 0 confidence.
    if (!(total > 0.0f)) return 0.0f;
    int lo = center - halfwidth;
    int hi = center + halfwidth;
    if (lo < 0) lo = 0;
    if (hi > n - 1) hi = n - 1;
    float window = 0.0f;
    for (int i = lo; i <= hi; i++) window += v[i];
    float frac = window / total;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    return frac;
}

inline TempoEstimator::Estimate TempoEstimator::observe(const float *onset_df, int n) {
    if (n > kWindowLen) n = kWindowLen;

    // 1) Local-mean removal, exactly as the reference pipeline did.
    std::memcpy(work_, onset_df, n * sizeof(float));
    adaptive_threshold(work_, n, thresh_scratch_);

    // 2) Balanced autocorrelation.
    balanced_acf(work_, n, acf_);

    // 3) Harmonic-template score over the BPM grid.
    float score_sum = 0.0f;
    for (int c = 0; c < kNumCandidates; c++) {
        const float bpm = kBpmMin + static_cast<float>(c);
        score_[c] = harmonic_score_at(acf_, n, bpm);
        score_sum += score_[c];
    }

    if (score_sum <= 1e-12f) {
        // Silence / no evidence: hold tempo, drop stability. state_ stays
        // untouched (evidence-free windows contribute nothing).
        stable_count_ = 0;
        return {current_bpm_, 0.0f, false};
    }

    // 4) Leaky integration of the prior-weighted, normalised score.
    for (int c = 0; c < kNumCandidates; c++) {
        const float bpm = kBpmMin + static_cast<float>(c);
        const float obs = (score_[c] / score_sum) * tempo_prior(bpm);
        state_[c] = kSmoothLambda * state_[c] + (1.0f - kSmoothLambda) * obs;
    }

    // 5) Argmax + parabolic sub-grid refinement.
    int best = 0;
    for (int c = 1; c < kNumCandidates; c++)
        if (state_[c] > state_[best]) best = c;
    float bpm_est = kBpmMin + static_cast<float>(best);
    if (best > 0 && best < kNumCandidates - 1) {
        bpm_est += parabolic_offset(state_[best - 1], state_[best], state_[best + 1]);
    }

    // 6) Confidence: peak-neighborhood mass fraction of the SMOOTHED state.
    //    Measures cross-window reproducibility of evidence — on noise the
    //    per-window peaks land at random candidates and average out, so no
    //    +-kConfHalfWidthBpm neighborhood accumulates mass.
    float conf = peak_mass_fraction(state_, kNumCandidates, best,
                                    kConfHalfWidthBpm) / kConfScale;
    if (conf > 1.0f) conf = 1.0f;

    // 7) Stability gate.
    if (last_argmax_bpm_ > 0.0f &&
        std::fabs(bpm_est - last_argmax_bpm_) <= kStableTolBpm) {
        stable_count_++;
    } else {
        stable_count_ = 1;
    }
    last_argmax_bpm_ = bpm_est;

    const bool locked = stable_count_ >= kStableUpdates;
    if (locked) current_bpm_ = bpm_est;
    const float conf_out = locked ? conf : 0.0f;
    return {current_bpm_, conf_out, locked};
}

}  // namespace audio_reactive
}  // namespace esphome
