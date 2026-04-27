#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <limits>

// BTrack-class beat tracker — clean re-implementation against the adamstark/BTrack
// reference (https://github.com/adamstark/BTrack). The previous port at this path
// drifted from the reference through multiple patches and locked on the wrong
// tempo on real music; see docs/plans/superpowers/specs/2026-04-27-btrack-
// reimplementation-analysis.md (in the aqara_advanced_lighting repo) for the
// post-mortem and per-step rebuild plan.
//
// Each algorithmic helper below is a public static method so unit tests can
// exercise it in isolation; see test/test_btrack/test_btrack.cpp for the
// per-helper coverage that drove the implementation. The BTrack class itself
// wraps stateful per-frame work (ring buffers, timers, Viterbi prior).
//
// Notable differences from the reference:
//   - Time-domain balanced ACF instead of FFT (simpler on ESP32-S3; output
//     is a constant factor 1/N below the reference's, irrelevant for argmax).
//   - adaptive_threshold uses the clamped window length as divisor at edges
//     (the reference reads x[-1] at i=8 — UB — and divides by a fixed 16,
//     producing a small DC residual; we clamp safely).
//   - All scratch buffers live as class members (the FFT task that calls
//     process() has only a 6 KB stack, no room for 2 KB stack-locals).

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

    // BPM detection range. The 41-candidate Viterbi grid (kTempoCandidates)
    // maps i=0..40 to lag = round(5168 / (2i+80)) — i.e. BPMs in 80..160 in
    // 2-BPM steps.
    static constexpr float kBpmMin = 80.0f;
    static constexpr float kBpmMax = 160.0f;

    // Lock / suppression thresholds used by process(). Externally-visible so
    // the audio_reactive component can publish "silenced" tempos as zero.
    static constexpr int   kWarmupFrames = 256;        // ~3s before lock can fire
    static constexpr int   kSilenceHoldFrames = 258;   // ~3s of silence → drop lock
    static constexpr float kLockConfidence = 0.4f;
    static constexpr float kSilenceConfidence = 0.3f;

    // Tempo Viterbi configuration.
    //   kTempoCandidates : 41-element grid, BPM = 80 + 2i (i=0..40 → 80..160)
    //   kTempoToLagFactor: 60 · 44100 / 512 — converts BPM to lag in frames
    //                      via lag = factor / BPM.
    //   kTempoSigma      : stddev (in candidate-index units) of the Gaussian
    //                      transition matrix. 41/8 = 5.125 matches the
    //                      adamstark reference.
    static constexpr int   kTempoCandidates = 41;
    static constexpr float kTempoToLagFactor = 5168.0f;
    static constexpr float kTempoSigma = 5.125f;

    // Comb filterbank size and predict_beat scratch sizes.
    //   kCombFbSize       : reference uses 128 — max beat period in frames.
    //   kMaxFutureFrames  : max frames predict_beat extrapolates ahead. At
    //                       60 BPM (slowest we'd see) bp ≈ 86 frames; round
    //                       up with margin to 220.
    //   kLogGaussMaxLen   : max length of log-Gaussian past window. The DP
    //                       window covers [bp/2, 2*bp] frames in the past
    //                       (length 1.5*bp + 1). At bp=86 that's 131; 256
    //                       provides plenty of headroom.
    static constexpr int   kCombFbSize = 128;
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
    float current_confidence() const { return current_confidence_; }

    // Read-only access to the Viterbi pipeline's intermediate buffers, so
    // the audio_reactive component can log top-N peak indices/values when
    // diagnosing which stage chose a particular tempo.
    //   acf()        : kHistoryLen entries, balanced ACF of onset DF
    //   comb_fb()    : kCombFbSize entries, comb-filterbank output
    //   delta()      : kTempoCandidates entries, current Viterbi delta
    //   prev_delta() : kTempoCandidates entries, previous Viterbi delta
    const float *acf() const { return acf_; }
    const float *comb_fb() const { return comb_fb_; }
    const float *delta() const { return delta_; }
    const float *prev_delta() const { return prev_delta_; }

    // Find the top `n_peaks` largest entries in `buf[0..len-1]`. Writes
    // their indices into `out_indices[0..n_peaks-1]` (descending by value),
    // their values into `out_values[]`. n_peaks must be ≤ len.
    static void top_peaks(const float *buf, int len, int n_peaks,
                          int *out_indices, float *out_values);
    // Override Viterbi-derived beat_period (used by step-5 tests to feed
    // the DP path a known beat period without going through tempo
    // induction). The next per-beat tempo update will overwrite it.
    void debug_set_beat_period_frames(float bp) {
        if (bp < 1.0f) bp = 1.0f;
        beat_period_frames_val_ = bp;
    }

    // ------------------------------------------------------------------
    // Algorithmic helpers — public static for unit testability.
    // Each is documented at its definition below.
    // ------------------------------------------------------------------

    // adaptive_threshold(): subtract local moving-mean from x[0..N-1] and
    // clamp at zero, in-place. Used by calculateTempo() in the reference
    // BEFORE the autocorrelation step (to remove DC bias from the onset
    // detection function so beat-period peaks aren't swamped) and also on
    // the comb-filterbank output before Viterbi tempo selection.
    //
    // Window: 8 samples before, 7 samples after each position, clipped at
    // [0, N-1] at edges. The "mean" matches the reference's mean_array():
    // negatives contribute zero to the sum, but the divisor is the full
    // window length (so a window full of negatives produces a near-zero
    // threshold rather than a meaningless value).
    //
    // `scratch` must point to at least N floats; supplied by the caller so
    // production callers can route through a class-member buffer (FFT-task
    // stack budget is 6 KB, no room for 2 KB stack-locals here).
    static void adaptive_threshold(float *x, int N, float *scratch);

    // balanced_acf(): linear autocorrelation of `in[0..N-1]`, written to
    // `out[0..N-1]`. Each lag is normalised by the number of valid pairs
    // (N - lag) — i.e. the unbiased / "balanced" estimator. Without this
    // normalisation, small lags would dominate purely because more pairs
    // contribute to the sum.
    //
    // The reference computes this via FFT (FFT(|FFT(zero-padded x)|²) /
    // (N - lag)). We compute it directly in the time domain — O(N²), but
    // for N=512 that's 262 K multiplies per call, well within the FFT
    // task's per-beat budget on ESP32-S3. Output is a constant factor 1/N
    // below the reference's (FFT-based output is N × the time-domain
    // ACF); since downstream operations look at relative peaks, the
    // factor is irrelevant for tempo detection.
    //
    // `in` and `out` may NOT alias.
    static void balanced_acf(const float *in, int N, float *out);

    // rayleigh_weights(): fill out[0..N-1] with the Rayleigh distribution
    //   w[n] = (n / r²) · exp(-n² / 2r²)
    // which peaks at n = r and decays smoothly above and below. Used as the
    // tempo prior in the comb filterbank: r = 43 frames at 86.13 fps means
    // the prior peaks at ~120 BPM. w[0] = 0 (the n factor zeroes it).
    static void rayleigh_weights(float r, int N, float *out);

    // comb_filterbank(): given the balanced ACF of the onset DF, score each
    // candidate beat-period T by summing ACF energy at T and its first three
    // harmonics, weighted by the Rayleigh prior at T. Sharper peaks than the
    // raw ACF.
    //
    // For each period candidate i (0-based, frame value = i+1), the output is
    //   out[i] = w[i] · Σ_{a=1..4} (1 / (2a-1)) · Σ_{b=-(a-1)..(a-1)} acf[a*i + a + b - 1]
    // where indices in `acf` outside [0, acf_len) are treated as zero.
    //
    // The "+a-1" offset comes from the reference's 1-based-to-0-based
    // translation (a known off-by-one in adamstark/BTrack; harmless because
    // it shifts every candidate uniformly so argmax is unaffected).
    static void comb_filterbank(const float *acf, int acf_len,
                                const float *weights, int n_periods,
                                float *out);

    // build_tempo_observation(): for each candidate i in [0, n_candidates),
    // sample the comb filterbank at the period for BPM = 80 + 2i, plus the
    // period for double-tempo BPM = 160 + 4i, and write the sum to obs[i].
    //
    //   lag_base   = round(kTempoToLagFactor / (2i + 80))
    //   lag_double = round(kTempoToLagFactor / (4i + 160))
    //   obs[i] = comb_fb[lag_base - 1] + comb_fb[lag_double - 1]
    //
    // The "(lag - 1)" indexing replicates the reference's 1-based-to-0-based
    // off-by-one (consistent with comb_filterbank's same offset, so ACF
    // peaks land at the right candidate).
    static void build_tempo_observation(const float *comb_fb, int n_periods,
                                        float *obs, int n_candidates);

    // build_tempo_transition_matrix(): fill tm[n×n] (row-major) with the
    // Gaussian-in-candidate-index-space transition matrix used by the
    // Viterbi step. tm[i*n + j] = exp(-0.5 * ((j - i)/sigma)²). Unnormalised.
    static void build_tempo_transition_matrix(float sigma, int n_candidates,
                                              float *tm);

    // viterbi_step(): one Viterbi forward update.
    //   delta_out[j] = obs[j] * max_i(prev_delta[i] * tm[i*n + j])
    // Output is normalised to sum to 1 (so successive calls don't drift to
    // zero/inf). prev_delta and delta_out may NOT alias.
    static void viterbi_step(const float *prev_delta, const float *obs,
                             const float *tm, int n_candidates,
                             float *delta_out);

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

    // State-persistence soft blend (deviation from reference, added 2026-04-27
    // after hardware testing showed brief low-onset-strength frames could
    // corrupt the Viterbi prior and lock the system on a wrong tempo).
    //
    // The reference's calculateTempo() unconditionally replaces prev_delta
    // with the new delta on every step. On real-time mic-captured audio
    // that fails: a single noisy frame produces a noisy delta which then
    // becomes the prior for subsequent (now mis-anchored) frames, eventually
    // building up a strong "wrong" lock that survives even when good
    // observations resume.
    //
    // The reverse problem — a hard gate that simply rejects low-confidence
    // updates — also fails: it breaks the bootstrap feedback loop where
    // each frame's delta sharpens prev_delta and thereby sharpens the
    // next delta. Without that loop the system never escapes a uniform
    // prior on real music (which often has only mid-confidence locks).
    //
    // Soft blend resolves both: prev_delta_ is updated on every step, but
    // the per-step blend weight equals the new confidence (clamped to a
    // small floor so a sustained-silence prior doesn't become arbitrarily
    // sticky). High-conf updates dominate; low-conf transients only shift
    // the prior by a small fraction, leaving the lock mostly intact.
    //
    //   kPriorBlendFloor : minimum blend fraction; ensures prev_delta_
    //                       continues to evolve even when conf collapses
    //                       (e.g., during a real tempo change). With 0.03
    //                       the prior still drifts ~3% toward the new
    //                       delta per beat in the worst case, so a genuine
    //                       tempo shift unlocks within ~30 beats.
    static constexpr float kPriorBlendFloor = 0.03f;

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

    // ---- Beat-prediction timers (count down per frame) ----
    int   time_to_next_beat_{-1};       // frames until next predicted beat (0 = beat fires this frame)
    int   time_to_next_prediction_{10}; // frames until next predict_beat() call

    // ---- Precomputed lookup tables (initialised lazily on first reset) ----
    bool  tables_init_{false};
    float weighting_vector_[kCombFbSize]{};                       // Rayleigh tempo prior
    float transition_matrix_[kTempoCandidates * kTempoCandidates]{};
    float prev_delta_[kTempoCandidates]{};

    // ---- Scratch buffers (class members, not stack locals — FFT task has 6 KB stack) ----
    float threshold_scratch_[kHistoryLen]{};                      // adaptive_threshold scratch
    float acf_buf_[kHistoryLen]{};                                // time-ordered onset DF for ACF
    float acf_[kHistoryLen]{};                                    // ACF output
    float comb_fb_[kCombFbSize]{};
    float tempo_obs_[kTempoCandidates]{};
    float delta_[kTempoCandidates]{};
    float log_gauss_scratch_[kLogGaussMaxLen]{};                  // for cumulative_score / predict_beat
    float future_scratch_[kHistoryLen + kMaxFutureFrames]{};      // linearised cumulative_score + future

    // ---- Internal helpers ----
    void init_tables_();
    void update_cumulative_score_(float onset);
    void predict_beat_();
    // Run one full tempo-induction pass: linearise onset_history_,
    // adaptive_threshold → balanced_acf → comb_filterbank →
    // adaptive_threshold → build_tempo_observation → viterbi_step,
    // then derive BPM and confidence from the argmax of the new delta.
    // Per the reference, this runs once per beat (when time_to_next_beat
    // hits zero in process()).
    void update_tempo_estimate_();
};

// ----------------------------------------------------------------------
// Implementation — single-header inline.
// ----------------------------------------------------------------------

inline void BTrack::init_tables_() {
    rayleigh_weights(43.0f, kCombFbSize, weighting_vector_);
    build_tempo_transition_matrix(kTempoSigma, kTempoCandidates, transition_matrix_);
    for (int i = 0; i < kTempoCandidates; i++) prev_delta_[i] = 1.0f;  // uniform initial prior
    tables_init_ = true;
}

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
    if (!tables_init_) {
        init_tables_();
    } else {
        // Re-seed Viterbi prior to uniform 1.0 (matches reset semantics).
        for (int i = 0; i < kTempoCandidates; i++) prev_delta_[i] = 1.0f;
    }
}

inline BTrack::Result BTrack::process(float onset_strength) {
    if (!tables_init_) init_tables_();

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
    //    expires; the per-beat tempo induction (Viterbi over ACF →
    //    comb FB) runs when the beat timer reaches zero — that's the
    //    reference's event-driven cadence (vs. the broken every-43-frames
    //    schedule in the previous port).
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
    // 1) Linearise onset_history_ ring into time-order. After process()
    //    advanced history_write_, ring[history_write_] is the oldest,
    //    ring[history_write_-1 mod N] is the newest.
    for (int i = 0; i < kHistoryLen; i++) {
        acf_buf_[i] = onset_history_[(history_write_ + i) % kHistoryLen];
    }

    // 2) Subtract local moving-mean from the onset DF before ACF — this is
    //    the load-bearing fix that was missing/misordered in the previous
    //    port. Without it the always-positive SuperFlux baseline produces
    //    a DC bias that swamps all beat-period peaks in the ACF.
    adaptive_threshold(acf_buf_, kHistoryLen, threshold_scratch_);

    // 3) Balanced autocorrelation.
    balanced_acf(acf_buf_, kHistoryLen, acf_);

    // 4) Comb filterbank with Rayleigh prior.
    comb_filterbank(acf_, kHistoryLen, weighting_vector_, kCombFbSize, comb_fb_);

    // 5) Threshold the comb FB output (matches reference order).
    adaptive_threshold(comb_fb_, kCombFbSize, threshold_scratch_);

    // 6) Build the 41-element tempo observation vector.
    build_tempo_observation(comb_fb_, kCombFbSize, tempo_obs_, kTempoCandidates);

    // 7) Viterbi forward step.
    viterbi_step(prev_delta_, tempo_obs_, transition_matrix_,
                 kTempoCandidates, delta_);

    // Detect silence / no-information frames: if the observation was all-zero
    // the Viterbi product collapses to zero and we'd lock onto whatever
    // (uniform) max_i path won the tie. Re-seed prev_delta to uniform so
    // the next non-silent window can lock from scratch.
    float delta_sum = 0.0f;
    for (int j = 0; j < kTempoCandidates; j++) delta_sum += delta_[j];
    if (delta_sum < 1e-9f) {
        for (int j = 0; j < kTempoCandidates; j++) prev_delta_[j] = 1.0f;
        current_confidence_ = 0.0f;
        return;
    }

    // 8) Argmax → BPM.
    int best_idx = 0;
    float best_val = -1.0f;
    for (int j = 0; j < kTempoCandidates; j++) {
        if (delta_[j] > best_val) { best_val = delta_[j]; best_idx = j; }
    }
    const float bpm = 80.0f + 2.0f * static_cast<float>(best_idx);
    current_bpm_ = bpm;
    beat_period_frames_val_ = kFrameHz * 60.0f / bpm;

    // 9) Confidence: peak-neighbourhood mass relative to the uniform floor.
    //    Sharp unimodal → conf ≈ 1; flat → conf ≈ 0.
    float peak_mass = 0.0f;
    for (int j = best_idx - 2; j <= best_idx + 2; j++) {
        if (j >= 0 && j < kTempoCandidates) peak_mass += delta_[j];
    }
    const float uniform_mass = 5.0f / static_cast<float>(kTempoCandidates);
    float conf = (peak_mass - uniform_mass) / (1.0f - uniform_mass);
    if (conf < 0.0f) conf = 0.0f;
    if (conf > 1.0f) conf = 1.0f;
    current_confidence_ = conf;

    // 10) Soft-blend prev_delta_. blend ∝ confidence so:
    //       - High-conf frames update prev_delta_ almost fully (matches
    //         reference behaviour on clean signals).
    //       - Low-conf transients only nudge the prior by a small fraction,
    //         leaving an established lock mostly intact across one bad frame.
    //       - The kPriorBlendFloor keeps progress nonzero so a genuine
    //         tempo change (sustained low conf at the new tempo) eventually
    //         takes over.
    float blend = conf;
    if (blend < kPriorBlendFloor) blend = kPriorBlendFloor;
    if (blend > 1.0f) blend = 1.0f;
    for (int j = 0; j < kTempoCandidates; j++) {
        prev_delta_[j] = blend * delta_[j] + (1.0f - blend) * prev_delta_[j];
    }
}

inline void BTrack::adaptive_threshold(float *x, int N, float *scratch) {
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

inline void BTrack::rayleigh_weights(float r, int N, float *out) {
    if (N <= 0) return;
    if (r <= 0.0f) {
        for (int i = 0; i < N; i++) out[i] = 0.0f;
        return;
    }
    const float two_r2 = 2.0f * r * r;
    const float inv_r2 = 1.0f / (r * r);
    for (int n = 0; n < N; n++) {
        const float nf = static_cast<float>(n);
        out[n] = (nf * inv_r2) * std::exp(-(nf * nf) / two_r2);
    }
}

inline void BTrack::comb_filterbank(const float *acf, int acf_len,
                                    const float *weights, int n_periods,
                                    float *out) {
    constexpr int kNumElements = 4;
    for (int i = 0; i < n_periods; i++) out[i] = 0.0f;
    if (n_periods < 3) return;  // no valid candidates

    // Reference loops 1-based i ∈ [2, n_periods-1]; in 0-based that's
    // i ∈ [1, n_periods-2]. The off-by-one in the lag index (a*i + a + b - 1
    // rather than a*i + a + b) is replicated faithfully — it's a known
    // adamstark/BTrack quirk that shifts every candidate uniformly so
    // argmax is unaffected.
    for (int i = 1; i <= n_periods - 2; i++) {
        const float w = weights[i];
        if (w == 0.0f) continue;
        for (int a = 1; a <= kNumElements; a++) {
            const float scale = w / static_cast<float>(2 * a - 1);
            for (int b = 1 - a; b <= a - 1; b++) {
                const int lag_idx = a * (i + 1) + b - 1;  // 1-based: a*i + b
                if (lag_idx < 0 || lag_idx >= acf_len) continue;
                out[i] += acf[lag_idx] * scale;
            }
        }
    }
}

inline void BTrack::build_tempo_observation(const float *comb_fb, int n_periods,
                                            float *obs, int n_candidates) {
    // Deviation from the reference: we sample only the lag1 ("base period")
    // location in the comb filterbank and skip the lag2 ("double-tempo")
    // sample that the reference adds. Hardware diagnostics on a 152-BPM song
    // showed the lag2 sum spuriously favouring candidate i=17 (114 BPM):
    // its lag1 (45) read comb_fb[44] (catching the music's 4x ACF harmonic
    // via comb_filterbank's a=3 path) AND its lag2 (23) read comb_fb[22]
    // (catching the 2x ACF harmonic via the a=3 path again). The 152-BPM
    // candidate (i=35) only had its lag1 land on a comb_fb peak; its lag2
    // (300 BPM) hit nothing. The dual-sampling thus inherited harmonic mass
    // from the correct tempo into the wrong candidate.
    //
    // The reference includes lag2 to disambiguate octave-confusion cases
    // (where the music's perceived tempo is the half of its strongest ACF
    // peak). On music with strong integer-harmonic structure that protection
    // backfires; we keep lag1-only as our default. Reintroducing lag2
    // (perhaps as a low-weight contribution rather than a full add) is a
    // future tuning lever if half-tempo lock-in becomes a problem.
    for (int i = 0; i < n_candidates; i++) {
        const float bpm_base = static_cast<float>(2 * i + 80);
        int lag1 = static_cast<int>(std::round(kTempoToLagFactor / bpm_base));
        // Clamp to [1, n_periods] so (lag - 1) is a valid 0-based index.
        if (lag1 < 1) lag1 = 1;
        if (lag1 > n_periods) lag1 = n_periods;
        obs[i] = comb_fb[lag1 - 1];
    }
}

inline void BTrack::build_tempo_transition_matrix(float sigma, int n_candidates,
                                                  float *tm) {
    if (sigma <= 0.0f) sigma = 1e-3f;  // defensive — divide-by-zero guard
    const float two_sigma2 = 2.0f * sigma * sigma;
    for (int i = 0; i < n_candidates; i++) {
        for (int j = 0; j < n_candidates; j++) {
            const float diff = static_cast<float>(j - i);
            tm[i * n_candidates + j] = std::exp(-(diff * diff) / two_sigma2);
        }
    }
}

inline void BTrack::viterbi_step(const float *prev_delta, const float *obs,
                                 const float *tm, int n_candidates,
                                 float *delta_out) {
    // Forward pass: delta[j] = obs[j] * max_i(prev_delta[i] * tm[i][j]).
    for (int j = 0; j < n_candidates; j++) {
        float best = 0.0f;
        for (int i = 0; i < n_candidates; i++) {
            const float v = prev_delta[i] * tm[i * n_candidates + j];
            if (v > best) best = v;
        }
        delta_out[j] = obs[j] * best;
    }
    // Normalise — keeps successive Viterbi steps numerically stable. If the
    // observation is all-zero (silence) the sum is 0 and we can't normalise;
    // leave as zeros and let the caller decide what to do (typically reset
    // prev_delta back to uniform).
    float sum = 0.0f;
    for (int j = 0; j < n_candidates; j++) sum += delta_out[j];
    if (sum > 0.0f) {
        const float inv_sum = 1.0f / sum;
        for (int j = 0; j < n_candidates; j++) delta_out[j] *= inv_sum;
    }
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

inline void BTrack::top_peaks(const float *buf, int len, int n_peaks,
                              int *out_indices, float *out_values) {
    // Initialize outputs to "empty" sentinels.
    for (int i = 0; i < n_peaks; i++) {
        out_indices[i] = -1;
        out_values[i] = -std::numeric_limits<float>::infinity();
    }
    // Single-pass: for each input value, insert into the sorted top-N if
    // it beats the smallest current entry. n_peaks is small (typically 3),
    // so the inner loop is cheap.
    for (int i = 0; i < len; i++) {
        const float v = buf[i];
        if (v <= out_values[n_peaks - 1]) continue;
        // Find insertion position (descending order).
        int pos = n_peaks - 1;
        while (pos > 0 && v > out_values[pos - 1]) pos--;
        // Shift smaller values down.
        for (int k = n_peaks - 1; k > pos; k--) {
            out_values[k] = out_values[k - 1];
            out_indices[k] = out_indices[k - 1];
        }
        out_values[pos] = v;
        out_indices[pos] = i;
    }
}

inline void BTrack::balanced_acf(const float *in, int N, float *out) {
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

}  // namespace audio_reactive
}  // namespace esphome
