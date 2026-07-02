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
//   - Confidence is evidence-based: peak-to-median ratio of the RAW score
//     vector (before prior and smoothing), gated by argmax stability across
//     consecutive updates. Non-rhythmic input produces an unstable argmax
//     and near-flat scores -> confidence 0.
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
    //   kSmoothLambda   : per-update state retention. Updates run once per
    //                     beat (~2/s at 120 BPM); 0.85^10 = 0.20, so a wrong
    //                     lock decays to noise within ~10 beats (~5 s).
    //   kStableUpdates  : consecutive agreeing updates before "locked".
    //   kStableTolBpm   : agreement tolerance between successive argmaxes.
    //   kConfEmaAlpha   : EMA rate for the evidence confidence.
    static constexpr float kSmoothLambda  = 0.85f;
    static constexpr int   kStableUpdates = 4;
    static constexpr float kStableTolBpm  = 3.0f;
    static constexpr float kConfEmaAlpha  = 0.3f;

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

    // Peak-to-median confidence of a raw score vector: 0 for flat input,
    // -> 1 for a single dominant peak. `scratch` needs n floats.
    static float raw_confidence(const float *score, int n, float *scratch);

    // Log-normal prior weight at `bpm`.
    static float tempo_prior(float bpm);

    // Parabolic refinement of an argmax on a uniform grid: given values at
    // (idx-1, idx, idx+1) returns the sub-grid offset in [-0.5, 0.5].
    static float parabolic_offset(float ym1, float y0, float yp1);

 private:
    // Smoother state.
    float state_[kNumCandidates]{};
    float conf_ema_{0.0f};
    float last_argmax_bpm_{0.0f};
    int   stable_count_{0};
    float current_bpm_{120.0f};

    // Scratch (class members - FFT task stack is 6 KB).
    float work_[kWindowLen]{};
    float thresh_scratch_[kWindowLen]{};
    float acf_[kWindowLen]{};
    float score_[kNumCandidates]{};
    float conf_scratch_[kNumCandidates]{};
};

// Definition for the in-class constexpr array (required at C++14/17 for ODR
// use; harmless in C++17 single-header context).
inline constexpr float TempoEstimator::kHarmonicWeights[TempoEstimator::kNumHarmonics];

inline void TempoEstimator::reset() {
    for (int i = 0; i < kNumCandidates; i++) state_[i] = 0.0f;
    conf_ema_ = 0.0f;
    last_argmax_bpm_ = 0.0f;
    stable_count_ = 0;
    current_bpm_ = 120.0f;
}

}  // namespace audio_reactive
}  // namespace esphome
