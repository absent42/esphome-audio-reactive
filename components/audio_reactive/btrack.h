#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>

#include "tempo_estimator.h"

// BTrack-class beat tracker — clean re-implementation against the adamstark/BTrack
// reference (https://github.com/adamstark/BTrack). The previous port at this path
// drifted from the reference through multiple patches and locked on the wrong
// tempo on real music; see docs/plans/superpowers/specs/2026-04-27-btrack-
// reimplementation-analysis.md (in the aqara_advanced_lighting repo) for the
// post-mortem and per-step rebuild plan.
//
// Tempo induction deliberately diverges from the reference: the former
// ACF -> comb-filterbank -> 41-candidate Viterbi chain is replaced by
// TempoEstimator (tempo_estimator.h), which scores a continuous 60-180 BPM
// grid with a fractional-lag harmonic template and smooths with a leaky
// integrator (see that header for the design rationale and
// docs/plans/2026-07-02-tempo-induction-rewrite.md for the audit that
// motivated the change). BTrack keeps the reference's onset-history ring,
// cumulative-score DP, and beat prediction, and delegates the per-beat
// tempo update to the estimator.
//
// The remaining algorithmic helper (log_gaussian_weights) is a public static
// method so unit tests can exercise it in isolation; see
// test/test_btrack/test_btrack.cpp.
//
// All scratch buffers live as class members (the FFT task that calls
// process() has only a 6 KB stack, no room for 2 KB stack-locals).

namespace esphome {
namespace audio_reactive {

class BTrack {
 public:
    // Frame rate of the onset-detection-function stream BTrack consumes.
    // 44100 Hz / 512-sample hop = 86.13 frames/s.
    static constexpr float kFrameHz = 86.13f;

    // Onset DF ring length. 512 samples ≈ 5.95s of history at kFrameHz.
    // Matches the reference's onsetDFBufferSize at hopSize=512.
    static constexpr int kHistoryLen = 512;

    // BPM detection range. Mirrors TempoEstimator::kBpmMin / kBpmMax (the
    // estimator's continuous 1-BPM candidate grid).
    static constexpr float kBpmMin = 60.0f;
    static constexpr float kBpmMax = 180.0f;

    // Lock / suppression thresholds used by process(). Externally-visible so
    // the audio_reactive component can publish "silenced" tempos as zero.
    static constexpr int   kWarmupFrames = 256;        // ~3s: confidence forced to 0 after reset
    static constexpr float kSilenceConfidence = 0.3f;

    // predict_beat scratch sizes.
    //   kMaxFutureFrames  : max frames predict_beat extrapolates ahead. At
    //                       60 BPM (slowest we'd see) bp ≈ 86 frames; round
    //                       up with margin to 220.
    //   kLogGaussMaxLen   : max length of log-Gaussian past window. The DP
    //                       window covers [bp/2, 2*bp] frames in the past
    //                       (length 1.5*bp + 1). At bp=86 that's 131; 256
    //                       provides plenty of headroom.
    static constexpr int   kMaxFutureFrames = 220;
    static constexpr int   kLogGaussMaxLen = 256;

    struct Result {
        float bpm;            // current tempo estimate
        float beat_phase;     // 0..1, wraps each beat
        float confidence;     // 0..1
        bool  beat_event;     // true on the frame the algorithm predicts a beat
    };

    BTrack() { reset(); }

    Result process(float onset_strength);
    void reset();
    float last_bpm() const { return current_bpm_; }

    // ------------------------------------------------------------------
    // Diagnostic / test-only accessors. Not part of the production API
    // contract — exposed for unit tests of the per-frame state machine.
    // ------------------------------------------------------------------
    int time_to_next_beat() const { return time_to_next_beat_; }
    int time_to_next_prediction() const { return time_to_next_prediction_; }
    float beat_period_frames() const { return beat_period_frames_val_; }
    // Override the estimator-derived beat_period (used by DP-path tests to
    // feed a known beat period without going through tempo induction). The
    // next per-beat tempo update will overwrite it.
    void debug_set_beat_period_frames(float bp) {
        if (bp < 1.0f) bp = 1.0f;
        beat_period_frames_val_ = bp;
    }

    // ------------------------------------------------------------------
    // Algorithmic helpers — public static for unit testability.
    // ------------------------------------------------------------------

    // log_gaussian_weights(): build the log-Gaussian transition window used
    // by the cumulative-score DP step and predict_beat.
    //
    //   v starts at -2 · beat_period; for each sample i in [0, n):
    //     w[i] = exp(-0.5 · (tightness · log(-v / beat_period))²)
    //     v += 1
    //
    // The peak (w = 1) occurs at v = -beat_period, i.e. index ≈ beat_period.
    // Below -2·beat_period the input to log() goes negative; the function
    // returns 0 for those positions to avoid NaN.
    static void log_gaussian_weights(float beat_period, float tightness,
                                     int n, float *out);

    // Cumulative-score parameters.
    //   kAlpha     : past/present blend in the DP step (reference: 0.9)
    //   kTightness : log-Gaussian tightness (reference: 5)
    static constexpr float kAlpha = 0.9f;
    static constexpr float kTightness = 5.0f;

 private:
    // ---- Stateful per-frame work ----
    int   history_write_{0};            // ring head (next slot to write)
    int   frames_since_reset_{0};
    float onset_history_[kHistoryLen]{};       // onset DF ring (oldest at history_write_, newest at history_write_-1)
    float cumulative_score_[kHistoryLen]{};    // DP ring (same indexing as onset_history_)

    // ---- Tempo state ----
    float current_bpm_{120.0f};
    float beat_phase_{0.0f};
    float current_confidence_{0.0f};
    float beat_period_frames_val_{kFrameHz * 60.0f / 120.0f};  // ≈43
    TempoEstimator tempo_est_;

    // ---- Beat-prediction timers (count down per frame) ----
    int   time_to_next_beat_{-1};       // frames until next predicted beat (0 = beat fires this frame)
    int   time_to_next_prediction_{10}; // frames until next predict_beat() call

    // ---- Scratch buffers (class members, not stack locals — FFT task has 6 KB stack) ----
    float acf_buf_[kHistoryLen]{};                                // onset ring linearised into time-order
    float log_gauss_scratch_[kLogGaussMaxLen]{};                  // for cumulative_score / predict_beat
    float future_scratch_[kHistoryLen + kMaxFutureFrames]{};      // linearised cumulative_score + future

    // ---- Internal helpers ----
    void update_cumulative_score_(float onset);
    void predict_beat_();
    // Per-beat tempo induction: linearise onset_history_ into time-order
    // and delegate to TempoEstimator::observe(); adopt its estimate when
    // locked and force confidence to 0 during warmup. Per the reference's
    // cadence, this runs once per beat (when time_to_next_beat hits zero
    // in process()).
    void update_tempo_estimate_();
};

// TempoEstimator contract: BTrack hands its whole onset ring to
// TempoEstimator::observe() and publishes the estimator's BPM range, so the
// two classes' constants must agree (all are literals, so the float
// comparisons are exact).
static_assert(BTrack::kHistoryLen == TempoEstimator::kWindowLen,
              "BTrack ring length must match TempoEstimator window length");
static_assert(BTrack::kBpmMin == TempoEstimator::kBpmMin,
              "BTrack::kBpmMin must mirror TempoEstimator::kBpmMin");
static_assert(BTrack::kBpmMax == TempoEstimator::kBpmMax,
              "BTrack::kBpmMax must mirror TempoEstimator::kBpmMax");
static_assert(BTrack::kFrameHz == TempoEstimator::kFrameHz,
              "BTrack::kFrameHz must match TempoEstimator::kFrameHz");

// ----------------------------------------------------------------------
// Implementation — single-header inline.
// ----------------------------------------------------------------------

inline void BTrack::reset() {
    history_write_ = 0;
    frames_since_reset_ = 0;
    for (int i = 0; i < kHistoryLen; i++) {
        onset_history_[i] = 0.0f;
        cumulative_score_[i] = 0.0f;
    }
    current_bpm_ = 120.0f;
    beat_phase_ = 0.0f;
    current_confidence_ = 0.0f;
    beat_period_frames_val_ = kFrameHz * 60.0f / 120.0f;  // ≈43
    time_to_next_beat_ = -1;
    time_to_next_prediction_ = 10;
    tempo_est_.reset();
}

inline BTrack::Result BTrack::process(float onset_strength) {
    // 1) Push onset into ring at history_write_.
    onset_history_[history_write_] = onset_strength;

    // 2) Update cumulative score at the same ring slot. Both arrays now
    //    align time-wise.
    update_cumulative_score_(onset_strength);

    // 3) Advance ring head AFTER both writes — so next frame's "0 frames
    //    in the past" maps to this frame's slot.
    history_write_ = (history_write_ + 1) % kHistoryLen;
    frames_since_reset_++;

    // 4) Tick timers. predict_beat() is called when the prediction timer
    //    expires; the per-beat tempo induction (TempoEstimator::observe)
    //    runs when the beat timer reaches zero — that's the reference's
    //    event-driven cadence (vs. the broken every-43-frames schedule in
    //    the previous port).
    time_to_next_prediction_--;
    time_to_next_beat_--;
    if (time_to_next_prediction_ <= 0) {
        predict_beat_();
    }
    if (time_to_next_beat_ == 0) {
        update_tempo_estimate_();
    }

    const bool beat_due = (time_to_next_beat_ == 0);

    // Beat phase: free-running between events; snaps to 0 on each beat.
    float bp = beat_period_frames_val_;
    if (bp < 1.0f) bp = 1.0f;
    if (beat_due) {
        beat_phase_ = 0.0f;
    } else {
        beat_phase_ += 1.0f / bp;
        if (beat_phase_ >= 1.0f) beat_phase_ -= 1.0f;
    }

    return { current_bpm_, beat_phase_, current_confidence_, beat_due };
}

inline void BTrack::update_cumulative_score_(float onset) {
    float bp = beat_period_frames_val_;
    if (bp < 2.0f) bp = 2.0f;

    // Window covers [bp/2, 2*bp] frames in the past (inclusive).
    const int win_far  = static_cast<int>(std::round(2.0f * bp));    // furthest in past
    const int win_near = static_cast<int>(std::round(0.5f * bp));    // closest in past
    int win_size = win_far - win_near + 1;
    if (win_size < 2 || win_far >= kHistoryLen) {
        // Insufficient history (early warmup) — store onset directly.
        cumulative_score_[history_write_] = onset;
        return;
    }
    if (win_size > kLogGaussMaxLen) win_size = kLogGaussMaxLen;

    log_gaussian_weights(bp, kTightness, win_size, log_gauss_scratch_);

    // Find max of (past_score * log_gauss_weight) over the window. Linear
    // index 0 of the log-Gaussian corresponds to v = -2bp (furthest past).
    float max_val = 0.0f;
    for (int i = 0; i < win_size; i++) {
        const int frames_back = win_far - i;     // i=0 → furthest past
        const int ring_idx = (history_write_ - frames_back + kHistoryLen) % kHistoryLen;
        const float v = cumulative_score_[ring_idx] * log_gauss_scratch_[i];
        if (v > max_val) max_val = v;
    }
    const float new_score = (1.0f - kAlpha) * onset + kAlpha * max_val;
    cumulative_score_[history_write_] = new_score;
}

inline void BTrack::predict_beat_() {
    float bp = beat_period_frames_val_;
    if (bp < 2.0f) bp = 2.0f;

    int future_window = static_cast<int>(std::round(bp));
    if (future_window < 2) future_window = 2;
    if (future_window > kMaxFutureFrames) future_window = kMaxFutureFrames;

    // 1) Linearise cumulative_score ring into future_scratch_[0 .. kHistoryLen-1]
    //    in time-order (oldest at 0, newest at kHistoryLen-1). After
    //    process() has advanced history_write_, that slot holds the
    //    OLDEST data we still have; the newest sits at history_write_-1.
    for (int i = 0; i < kHistoryLen; i++) {
        future_scratch_[i] = cumulative_score_[(history_write_ + i) % kHistoryLen];
    }

    // 2) Past-window log-Gaussian weights — same shape as in update_cumulative_score_,
    //    but reused to extrapolate FUTURE samples into future_scratch_[kHistoryLen..].
    const int win_far  = static_cast<int>(std::round(2.0f * bp));
    const int win_near = static_cast<int>(std::round(0.5f * bp));
    int win_size = win_far - win_near + 1;
    if (win_size < 2) {
        time_to_next_beat_ = static_cast<int>(std::round(bp * 0.5f));
        time_to_next_prediction_ = time_to_next_beat_ + static_cast<int>(std::round(bp * 0.5f));
        if (time_to_next_prediction_ < 1) time_to_next_prediction_ = 1;
        return;
    }
    if (win_size > kLogGaussMaxLen) win_size = kLogGaussMaxLen;
    log_gaussian_weights(bp, kTightness, win_size, log_gauss_scratch_);

    // 3) Synthesise future cumulative score (alpha=1, onset=0):
    //    future_cs[N+f] = max over i in [N+f-win_far, N+f-win_near] of
    //                       future_cs[i] * log_gauss[(N+f - win_far) - i offset]
    //    Equivalently: for each future frame f, slide the past window forward
    //    by f, take max(past_window * weights).
    for (int f = 0; f < future_window; f++) {
        const int lookback_far = kHistoryLen + f - win_far;   // furthest past index in linear array
        float max_val = 0.0f;
        for (int i = 0; i < win_size; i++) {
            const int idx = lookback_far + i;
            if (idx < 0 || idx >= kHistoryLen + future_window) continue;
            const float v = future_scratch_[idx] * log_gauss_scratch_[i];
            if (v > max_val) max_val = v;
        }
        future_scratch_[kHistoryLen + f] = max_val;  // alpha=1, onset=0
    }

    // 4) Beat-expectation Gaussian centred at bp/2 frames into the future,
    //    σ = bp/2. Pick the future frame f that maximises the product.
    const float half_bp = bp * 0.5f;
    const float two_sigma2 = 2.0f * half_bp * half_bp;
    int best_f = static_cast<int>(std::round(half_bp));
    float best_score = -1.0f;
    for (int f = 0; f < future_window; f++) {
        const float dx = (static_cast<float>(f + 1) - half_bp);
        const float gaussian = std::exp(-(dx * dx) / two_sigma2);
        const float w = future_scratch_[kHistoryLen + f] * gaussian;
        if (w > best_score) {
            best_score = w;
            best_f = f;
        }
    }

    time_to_next_beat_ = best_f;
    time_to_next_prediction_ = best_f + static_cast<int>(std::round(half_bp));
    if (time_to_next_prediction_ < 1) time_to_next_prediction_ = 1;
}

inline void BTrack::update_tempo_estimate_() {
    // Linearise onset_history_ ring into time-order (oldest first). After
    // process() advanced history_write_, ring[history_write_] is the oldest.
    for (int i = 0; i < kHistoryLen; i++) {
        acf_buf_[i] = onset_history_[(history_write_ + i) % kHistoryLen];
    }

    const auto est = tempo_est_.observe(acf_buf_, kHistoryLen);

    // Warmup: this gate covers the first ~3 s while the ring fills (a full
    // window takes ~6 s); estimates made on a mostly-zero ring are
    // unreliable, so suppress confidence (the component publishes 0 BPM
    // below kSilenceConfidence) while still letting the estimator's
    // internal state build up. The estimator's own stability gate covers
    // the remainder of the fill.
    if (frames_since_reset_ < kWarmupFrames) {
        current_confidence_ = 0.0f;
        return;
    }

    current_confidence_ = est.confidence;
    if (est.locked) {
        current_bpm_ = est.bpm;
        beat_period_frames_val_ = kFrameHz * 60.0f / est.bpm;
        if (beat_period_frames_val_ < 1.0f) beat_period_frames_val_ = 1.0f;
    }
    // When not locked, keep the previous beat period so the cumulative-score
    // DP and beat prediction stay coherent; confidence tells consumers not
    // to trust the tempo.
}

inline void BTrack::log_gaussian_weights(float beat_period, float tightness,
                                         int n, float *out) {
    if (n <= 0) return;
    if (beat_period < 1.0f) beat_period = 1.0f;
    float v = -2.0f * beat_period;
    for (int i = 0; i < n; i++) {
        const float ratio = -v / beat_period;
        if (ratio <= 0.0f) {
            // log() argument would be ≤ 0 — undefined. Force weight to 0
            // (defensive guard for v ≥ 0; doesn't fire in normal use because
            // v reaches 0 only at i = 2*beat_period, beyond the typical
            // window length).
            out[i] = 0.0f;
        } else {
            const float a = tightness * std::log(ratio);
            out[i] = std::exp(-0.5f * a * a);
        }
        v += 1.0f;
    }
}

}  // namespace audio_reactive
}  // namespace esphome
