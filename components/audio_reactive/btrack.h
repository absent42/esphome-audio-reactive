#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <numeric>

namespace esphome {
namespace audio_reactive {

/// BTrack-class beat tracker.
/// Input: onset-strength stream (86 Hz at 44.1 kHz / 2048-pt FFT / 512-hop).
/// Outputs per-frame: bpm, beat_phase [0,1], beat_confidence [0,1], beat_event.
///
/// The tempo-induction and DP beat-phase algorithms are ported from Adam Stark's
/// BTrack implementation (MIT-licensed, https://github.com/adamstark/BTrack),
/// which in turn is a re-implementation of:
///
///   Stark, Davies and Plumbley (2009) - "Real-Time Beat-Synchronous Analysis of
///   Musical Audio", DAFx 2009.
///
/// Adaptations for this project:
///   - Input is a scalar onset-strength stream from SuperFlux, so the reference's
///     spectral-flux / resample-to-512 pipeline is bypassed; our onset history
///     matches the reference buffer cadence (~86 Hz, hop=512 @ 44.1 kHz).
///   - Project-specific event gating (cold-start warm-up, lock-window,
///     silence-hold, fast silence detection via zero_onset_streak_) wraps the
///     DP output; these are NOT part of the BTrack algorithm.
///   - Tempo estimation runs on a fixed 43-frame cadence rather than
///     per-beat, so behavior stays stable across silence and cold-start.
class BTrack {
 public:
    static constexpr uint16_t kHistoryLen = 512;       // ~5.95s @ 86Hz (== BTrack onsetDFBufferSize)
    static constexpr float   kFrameHz     = 86.13f;    // 44100 / 512
    // BPM detection range. The 41-candidate grid (kTempoCandidates) maps
    // i=0..40 to lag = round(5168 / (2i + 80)) — i.e. BPMs 80..160 in 2-BPM
    // steps. Tempos outside this range can't be detected: a 60-BPM ballad
    // resolves to ~80 BPM, a 180-BPM track to ~160. The constants below
    // reflect what the grid actually covers; anything else would silently
    // misreport. Expanding the grid (step 3 over [60,180], or 61 candidates)
    // is a wider-impact change touching the Rayleigh prior + Viterbi arrays;
    // documenting the real range is the conservative fix until that lands.
    static constexpr float   kBpmMin      = 80.0f;
    static constexpr float   kBpmMax      = 160.0f;
    static constexpr float   kBpmPriorCenter = 120.0f;
    static constexpr uint16_t kWarmupFrames = 256;     // ~3s before confidence ramps
    static constexpr uint16_t kMinLockFrames = 86;     // ~1s of confidence >= threshold for events
    static constexpr float   kLockConfidence = 0.4f;
    static constexpr float   kSilenceConfidence = 0.3f;
    static constexpr uint16_t kSilenceHoldFrames = 258;  // ~3s

    // Comb filterbank / Viterbi tempo-tracking parameters (mirrors reference).
    static constexpr uint16_t kCombFbSize = 128;       // max beat period in frames
    static constexpr uint16_t kTempoCandidates = 41;   // reference uses 41 candidate tempi
    static constexpr float   kRayleighParameter = 43.0f;   // peak of Rayleigh ~ 120 BPM @ 86 Hz
    static constexpr float   kTempoSigma = 41.0f / 8.0f;   // stddev for tempo transition matrix
    // Width of the INITIAL prior Gaussian (in candidate-index units) when
    // Viterbi state is seeded at boot, on reset, or when the comb FB
    // collapses to silence-noise. Deliberately much wider than kTempoSigma
    // so the initial bias toward 120 BPM (i=20) doesn't drown out
    // observations for tempos far from 120. With kTempoSigma's value (5.125)
    // an actual 146 BPM song (i=33, dist 13 from centre) would start with
    // ~21x lower weight than 114 BPM (i=17, dist 3), and the comb-filter-
    // bank observation has to overcome that 21x bias before Viterbi flips —
    // in practice the lock on 114 wins. With kInitialPriorSigma=15 the
    // ratio drops to ~2x, leaving observations to determine the lock.
    static constexpr float   kInitialPriorSigma = 15.0f;
    static constexpr float   kTightness = 5.0f;            // log-gaussian tightness (reference: 5)
    static constexpr float   kAlpha = 0.9f;                // cumulative-score past/present blend

    struct Result {
        float bpm;            // held at last or 120 during warmup
        float beat_phase;     // 0..1, wraps each beat
        float confidence;     // 0..1
        bool beat_event;      // true on the frame where beat_phase wraps
    };

    Result process(float onset_strength);
    void reset();

    float last_bpm() const { return current_bpm_; }

 protected:
    // Onset-strength ring buffer (size kHistoryLen).
    float onset_history_[kHistoryLen]{};
    uint16_t history_write_{0};
    uint16_t frames_since_reset_{0};

    // Tempo state
    float current_bpm_{kBpmPriorCenter};
    float current_confidence_{0.0f};
    // beat period in frames (float to avoid quantisation at high BPM)
    float beat_period_frames_val_{kFrameHz * 60.0f / kBpmPriorCenter};

    // Beat-phase accumulator — driven by DP predictor when locked, otherwise
    // advances at 1/beat_period per frame as a free-running fallback.
    float beat_phase_{0.0f};
    uint16_t frames_above_lock_{0};
    uint16_t frames_below_lock_{0};
    // Count of consecutive near-zero onset frames — used to fast-drop confidence
    // during silence without having to wait for the onset ring buffer (~5.95s) to
    // fully flush. Threshold of kZeroOnsetEps mirrors SuperFlux's no-event floor.
    uint16_t zero_onset_streak_{0};
    static constexpr float kZeroOnsetEps = 1e-4f;

    // Comb-filterbank / Viterbi tempo-tracking scratch + state.
    float acf_[kHistoryLen]{};                 // balanced autocorrelation
    float weighting_vector_[kCombFbSize]{};    // Rayleigh prior
    float comb_fb_out_[kCombFbSize]{};
    float tempo_obs_[kTempoCandidates]{};
    float delta_[kTempoCandidates]{};
    float prev_delta_[kTempoCandidates]{};
    float tempo_tm_[kTempoCandidates][kTempoCandidates]{};   // transition matrix
    bool  tempo_tables_init_{false};

    // DP cumulative-score state.
    float cumulative_score_[kHistoryLen]{};
    // timeToNextBeat counts down in frames — 0 on the frame a beat is "due".
    // Negative before the first prediction fires.
    int16_t time_to_next_beat_{-1};
    // timeToNextPrediction counts down to the next predictBeat() call.
    int16_t time_to_next_prediction_{10};

    // Scratch buffers — class members instead of stack locals because the
    // FFT task's stack is only 6144 bytes (xTaskCreatePinnedToCore in
    // audio_reactive.cpp). A single 2 KB stack-local would already eat a
    // third of that budget; multiple in the same call chain would overflow.
    //
    // Sizing rationale:
    //   future_cs_       — linearised cumulative score (kHistoryLen) + a
    //                      synthesised future window (max ~220 frames at
    //                      60 BPM). Used by predict_beat_().
    //   log_gauss_       — log-Gaussian weights for predict_beat_(). Max
    //                      past-window length is ~1.5 × max beat period
    //                      at 60 BPM ≈ 129 frames; 256 gives headroom.
    //                      Also reused by update_cumulative_score_() —
    //                      its win_size never exceeds ~1.5 × beat_period
    //                      ≈ 96 frames at 80 BPM.
    //   acf_buf_         — linearised onset history for autocorrelation
    //                      in calculate_balanced_acf_(). Size = kHistoryLen.
    //   threshold_scratch_ — local-mean buffer used by adaptive_threshold_().
    //                        Sized to the largest input it operates on
    //                        (kHistoryLen, since it runs on acf_).
    static constexpr uint16_t kMaxFutureFrames = 220;
    static constexpr uint16_t kLogGaussMaxLen = 256;
    float future_cs_[kHistoryLen + kMaxFutureFrames]{};
    float log_gauss_[kLogGaussMaxLen]{};
    float acf_buf_[kHistoryLen]{};
    float threshold_scratch_[kHistoryLen]{};

    // Algorithmic pieces (adamstark/BTrack port, adapted for our streaming input).
    void init_tempo_tables_();
    void update_tempo_estimate_();           // 43-frame cadence tempo induction
    void update_cumulative_score_(float onset);
    void predict_beat_();
    // Helpers
    void calculate_balanced_acf_();
    void calculate_comb_fb_();
    void adaptive_threshold_(float *x, uint16_t n);
    static float mean_slice_(const float *x, uint16_t lo, uint16_t hi);  // [lo,hi]
    static void normalise_(float *x, uint16_t n);
    static void make_log_gaussian_weights_(float *w, uint16_t n, float beat_period);
    // Compute cumulative-score value at a new position given the past ring.
    // windowStart/windowEnd indices into cumulative_score_ ring-space (linear offsets
    // from 'oldest'). Returns (1-alpha)*onset + alpha * max(past * log_gauss).
    static float cumulative_score_value_(const float *cs_linear, uint16_t buf_size,
                                         const float *log_gauss, int32_t start_idx,
                                         int32_t end_idx, float onset, float alpha);

    // Return period (frames/beat) for current_bpm_.
    float beat_period_frames_() const { return beat_period_frames_val_; }
};

// Implementation — inlined to keep single-header.

inline void BTrack::init_tempo_tables_() {
    // Rayleigh weighting vector: peaks at n = rayleighParameter (~120 BPM).
    for (uint16_t n = 0; n < kCombFbSize; n++) {
        float nf = static_cast<float>(n);
        weighting_vector_[n] = (nf / (kRayleighParameter * kRayleighParameter)) *
                               expf(-(nf * nf) / (2.0f * kRayleighParameter * kRayleighParameter));
    }
    // Tempo transition matrix: Gaussian in candidate-index space, centred on each
    // previous candidate. Reference uses a Gaussian with sigma ~= 5.125.
    for (uint16_t i = 0; i < kTempoCandidates; i++) {
        for (uint16_t j = 0; j < kTempoCandidates; j++) {
            float diff = static_cast<float>(j) - static_cast<float>(i);
            tempo_tm_[i][j] = expf(-0.5f * (diff * diff) / (kTempoSigma * kTempoSigma));
        }
    }
    // Seed prior as centred Gaussian on the 120-BPM candidate (index 20 for 41 cands).
    // Uses kInitialPriorSigma (much wider than kTempoSigma) so the initial bias
    // toward 120 BPM doesn't drown out observations for tempos far from 120 —
    // see kInitialPriorSigma's docstring for the math.
    for (uint16_t i = 0; i < kTempoCandidates; i++) {
        float diff = static_cast<float>(i) - 20.0f;
        prev_delta_[i] = expf(-0.5f * (diff * diff) / (kInitialPriorSigma * kInitialPriorSigma));
    }
    tempo_tables_init_ = true;
}

inline void BTrack::reset() {
    for (uint16_t i = 0; i < kHistoryLen; i++) {
        onset_history_[i] = 0.0f;
        cumulative_score_[i] = 0.0f;
    }
    history_write_ = 0;
    frames_since_reset_ = 0;
    current_bpm_ = kBpmPriorCenter;
    current_confidence_ = 0.0f;
    beat_period_frames_val_ = kFrameHz * 60.0f / kBpmPriorCenter;
    beat_phase_ = 0.0f;
    frames_above_lock_ = 0;
    frames_below_lock_ = 0;
    zero_onset_streak_ = 0;
    time_to_next_beat_ = -1;
    time_to_next_prediction_ = 10;
    if (!tempo_tables_init_) init_tempo_tables_();
    // Reset Viterbi prior using the wider kInitialPriorSigma so any tempo
    // can lock from observations without fighting an overly narrow prior
    // bias toward 120 BPM.
    for (uint16_t i = 0; i < kTempoCandidates; i++) {
        float diff = static_cast<float>(i) - 20.0f;
        prev_delta_[i] = expf(-0.5f * (diff * diff) / (kInitialPriorSigma * kInitialPriorSigma));
    }
}

inline BTrack::Result BTrack::process(float onset_strength) {
    if (!tempo_tables_init_) init_tempo_tables_();

    // Push into ring buffer.
    onset_history_[history_write_] = onset_strength;
    history_write_ = (history_write_ + 1) % kHistoryLen;
    frames_since_reset_++;

    // Silence fast-path: track consecutive near-zero onsets. The ACF-based
    // confidence would otherwise stay high until the ring buffer flushes (~5.95s).
    if (onset_strength < kZeroOnsetEps) {
        zero_onset_streak_++;
    } else {
        zero_onset_streak_ = 0;
    }

    // DP cumulative-score update (cheap per-frame work).
    update_cumulative_score_(onset_strength);

    // Countdown timers for beat prediction / arrival.
    time_to_next_prediction_--;
    time_to_next_beat_--;

    bool dp_beat_due = (time_to_next_beat_ == 0);

    // Predict the next beat when the prediction timer expires.
    if (time_to_next_prediction_ <= 0) {
        predict_beat_();
    }

    // Re-estimate tempo every ~0.5s (43 frames). Keeps behaviour stable across
    // silence; reference BTrack runs this per-beat which is noisy during silence.
    if (frames_since_reset_ % 43 == 0) {
        update_tempo_estimate_();
    }

    // Fast-path silence override. Reset current_bpm_ to the prior AND reseed
    // prev_delta_ with the wide initial prior so a stale pre-silence lock
    // doesn't bias the next non-silent window. Without the prev_delta_
    // reseed, Viterbi state survives the silence edge and propagates via the
    // (intentionally tight) transition matrix — which means a lock that
    // formed on the wrong candidate before silence will tend to re-form on
    // that same candidate after silence, defeating the wide initial prior.
    if (zero_onset_streak_ >= kSilenceHoldFrames) {
        current_confidence_ = 0.0f;
        current_bpm_ = kBpmPriorCenter;
        beat_period_frames_val_ = kFrameHz * 60.0f / kBpmPriorCenter;
        for (uint16_t i = 0; i < kTempoCandidates; i++) {
            float diff = static_cast<float>(i) - 20.0f;
            prev_delta_[i] = expf(-0.5f * (diff * diff) / (kInitialPriorSigma * kInitialPriorSigma));
        }
    }

    // Advance beat phase. During a strong DP lock we align the phase with the
    // DP prediction; otherwise it free-runs using the current beat period.
    float period = beat_period_frames_();
    if (period < 1.0f) period = 1.0f;
    beat_phase_ += 1.0f / period;

    bool event = false;
    if (dp_beat_due && current_confidence_ >= kLockConfidence) {
        // Snap phase to a beat on DP's predicted beat frame.
        beat_phase_ = 0.0f;
        event = true;
    } else if (beat_phase_ >= 1.0f) {
        beat_phase_ -= 1.0f;
        event = true;
    }

    // Cold-start + silence/lock suppression.
    bool cold_start = frames_since_reset_ < kWarmupFrames;
    if (cold_start) {
        current_confidence_ = 0.0f;
    }
    if (current_confidence_ >= kLockConfidence) {
        frames_above_lock_++;
        frames_below_lock_ = 0;
    } else {
        frames_below_lock_++;
        frames_above_lock_ = 0;
    }
    bool suppress_event = cold_start ||
                          frames_above_lock_ < kMinLockFrames ||
                          (current_confidence_ < kSilenceConfidence && frames_below_lock_ >= kSilenceHoldFrames);

    return { current_bpm_, beat_phase_, current_confidence_, event && !suppress_event };
}

// Compute balanced autocorrelation of onset_history_. Output is normalised so
// that acf_[lag] is the mean product of pairs separated by `lag` over the
// valid overlap region. Equivalent to reference calculateBalancedACF().
inline void BTrack::calculate_balanced_acf_() {
    // Linearise ring into time-ordered form: oldest at index 0, newest at N-1.
    // Uses the class-member acf_buf_ scratch (was a 2 KB stack-local) so this
    // function fits comfortably in the FFT task's 6 KB stack budget.
    for (uint16_t i = 0; i < kHistoryLen; i++) {
        acf_buf_[i] = onset_history_[(history_write_ + i) % kHistoryLen];
    }
    for (uint16_t lag = 0; lag < kHistoryLen; lag++) {
        float sum = 0.0f;
        uint16_t count = kHistoryLen - lag;
        for (uint16_t i = 0; i < count; i++) {
            sum += acf_buf_[i] * acf_buf_[i + lag];
        }
        acf_[lag] = (count > 0) ? (sum / static_cast<float>(count)) : 0.0f;
    }
}

// Comb filterbank output — ported directly from adamstark/BTrack
// calculateOutputOfCombFilterBank(). combFilterBankOutput[T] sums contributions
// from lags (T, 2T, 3T, 4T) with neighbourhood weighting, scaled by the
// Rayleigh prior at T. Sharper peaks than plain ACF.
inline void BTrack::calculate_comb_fb_() {
    for (uint16_t k = 0; k < kCombFbSize; k++) comb_fb_out_[k] = 0.0f;
    constexpr int kNumCombElements = 4;
    for (int i = 2; i <= kCombFbSize - 1; i++) {           // T = 2..127
        float w = weighting_vector_[i - 1];
        for (int a = 1; a <= kNumCombElements; a++) {
            for (int b = 1 - a; b <= a - 1; b++) {
                int lag_idx = a * i + b - 1;
                if (lag_idx < 0 || lag_idx >= static_cast<int>(kHistoryLen)) continue;
                comb_fb_out_[i - 1] += (acf_[lag_idx] * w) / static_cast<float>(2 * a - 1);
            }
        }
    }
}

inline float BTrack::mean_slice_(const float *x, uint16_t lo, uint16_t hi) {
    // [lo, hi] inclusive (reference uses 1-based inclusive; caller adjusts).
    if (hi < lo) return 0.0f;
    float sum = 0.0f;
    for (uint16_t i = lo; i <= hi; i++) sum += x[i];
    return sum / static_cast<float>(hi - lo + 1);
}

// Reference adaptiveThreshold(): subtract local mean (pre=8, post=7) and
// clamp at zero. Flattens broadband energy so peaks stand out.
inline void BTrack::adaptive_threshold_(float *x, uint16_t N) {
    constexpr int p_pre = 8;
    constexpr int p_post = 7;
    // Reuses the class-member threshold_scratch_ (was a 2 KB stack-local).
    // Sized to kHistoryLen which is the largest N this method ever runs on
    // (it's also called with N=kCombFbSize=128 — uses just a prefix slice).
    float *thresh = threshold_scratch_;
    for (uint16_t i = 0; i < N; i++) thresh[i] = 0.0f;
    int t = std::min<int>(N, p_post);
    for (int i = 0; i <= t; i++) {
        int k = std::min<int>(i + p_pre, N - 1);
        thresh[i] = mean_slice_(x, 0, static_cast<uint16_t>(k));
    }
    for (int i = t + 1; i < static_cast<int>(N) - p_post; i++) {
        thresh[i] = mean_slice_(x, static_cast<uint16_t>(i - p_pre), static_cast<uint16_t>(i + p_post));
    }
    for (int i = static_cast<int>(N) - p_post; i < static_cast<int>(N); i++) {
        int k = std::max<int>(i - p_post, 0);
        thresh[i] = mean_slice_(x, static_cast<uint16_t>(k), static_cast<uint16_t>(N - 1));
    }
    for (uint16_t i = 0; i < N; i++) {
        x[i] -= thresh[i];
        if (x[i] < 0.0f) x[i] = 0.0f;
    }
}

inline void BTrack::normalise_(float *x, uint16_t n) {
    float sum = 0.0f;
    for (uint16_t i = 0; i < n; i++) sum += x[i];
    if (sum > 0.0f) {
        for (uint16_t i = 0; i < n; i++) x[i] /= sum;
    }
}

// Tempo induction — comb filterbank + Viterbi over candidate tempi.
// Ported from reference calculateTempo(). Candidate i -> lag (2*i + 80),
// i.e. beat periods 80..160 frames → 32..64 BPM — wait, that's wrong range.
// Reference: tempoToLagFactor = 60 * 44100 / 512 = 5168.
// tempoIndex1 = round(tempoToLagFactor / (2*i + 80))
//   i=0 → 5168/80 = 65 (lag-idx, 1-based) → ~80 BPM
//   i=40 → 5168/160 = 32 → ~160 BPM
// But the RESAMPLED DF in reference is at 128 lags not our 512 — so indices are
// DIFFERENT. In reference they resample to 512 samples spanning 128 lag bins
// somehow. Here we use our raw onset buffer: lag is directly in our frames.
// At 86 Hz, 120 BPM → 43 frames; 60 BPM → 86 frames; 180 BPM → 28.7 frames.
// We keep the reference formula since our framerate matches (hop=512, Fs=44.1k).
inline void BTrack::update_tempo_estimate_() {
    constexpr float kTempoToLagFactor = 60.0f * 44100.0f / 512.0f;  // 5168.0, reference

    calculate_balanced_acf_();
    // Adaptive threshold on ACF first (only within the usable lag range).
    adaptive_threshold_(acf_, kHistoryLen);
    calculate_comb_fb_();
    adaptive_threshold_(comb_fb_out_, kCombFbSize);

    // Build tempo observation vector. Candidate i spans BPM ~ 5168/(2i+80).
    // i=0 → 64.6 BPM, i=20 → 43.06 frames → 120 BPM, i=40 → 32.3 frames → 160 BPM.
    for (uint16_t i = 0; i < kTempoCandidates; i++) {
        int lag1 = static_cast<int>(roundf(kTempoToLagFactor / static_cast<float>(2 * i + 80)));
        int lag2 = static_cast<int>(roundf(kTempoToLagFactor / static_cast<float>(4 * i + 160)));
        lag1 = std::max(1, std::min(lag1, static_cast<int>(kCombFbSize)));
        lag2 = std::max(1, std::min(lag2, static_cast<int>(kCombFbSize)));
        tempo_obs_[i] = comb_fb_out_[lag1 - 1] + comb_fb_out_[lag2 - 1];
    }

    // Viterbi step: delta[j] = obs[j] * max_i(prev_delta[i] * tm[i][j]).
    for (uint16_t j = 0; j < kTempoCandidates; j++) {
        float max_val = -1.0f;
        for (uint16_t i = 0; i < kTempoCandidates; i++) {
            float v = prev_delta_[i] * tempo_tm_[i][j];
            if (v > max_val) max_val = v;
        }
        delta_[j] = max_val * tempo_obs_[j];
    }
    // Guard: if the observation is all-zero (silence/warmup) the product delta_
    // collapses to zero and the Viterbi state can never recover. Detect that
    // case and re-seed prev_delta_ with a Gaussian prior centred at 120 BPM,
    // matching reset() behaviour.
    float delta_sum = 0.0f;
    for (uint16_t j = 0; j < kTempoCandidates; j++) delta_sum += delta_[j];
    if (delta_sum < 1e-9f) {
        // Same wide-prior re-seed as reset() / init_tempo_tables_(). Wider
        // than kTempoSigma so the next non-silent window can lock to any
        // tempo without fighting a narrow bias toward 120 BPM.
        for (uint16_t i = 0; i < kTempoCandidates; i++) {
            float diff = static_cast<float>(i) - 20.0f;
            prev_delta_[i] = expf(-0.5f * (diff * diff) / (kInitialPriorSigma * kInitialPriorSigma));
        }
        // Don't update tempo this round — no information to learn from.
        // Keep previous current_bpm_ and drop confidence.
        current_confidence_ = 0.0f;
        return;
    }

    normalise_(delta_, kTempoCandidates);

    // Argmax + runner-up for confidence.
    int best_idx = 0;
    float best_val = -1.0f;
    float second_val = 0.0f;
    for (uint16_t j = 0; j < kTempoCandidates; j++) {
        if (delta_[j] > best_val) {
            second_val = best_val;
            best_val = delta_[j];
            best_idx = j;
        } else if (delta_[j] > second_val) {
            second_val = delta_[j];
        }
        prev_delta_[j] = delta_[j];
    }

    // Map candidate index back to beat period (in frames) and BPM.
    float beat_period_frames = kTempoToLagFactor / static_cast<float>(2 * best_idx + 80);
    if (beat_period_frames < 1.0f) beat_period_frames = 1.0f;
    float bpm = kFrameHz * 60.0f / beat_period_frames;
    // Guard against candidate-index giving BPM below kBpmMin (shouldn't happen with
    // indices 0..40 → 64.6..160 BPM), but clamp defensively.
    if (bpm < kBpmMin) bpm = kBpmMin;
    if (bpm > kBpmMax) bpm = kBpmMax;

    current_bpm_ = bpm;
    beat_period_frames_val_ = kFrameHz * 60.0f / bpm;

    // Confidence: peak-neighbourhood mass (best ± 2 bins) vs mean floor.
    // True tempi often land between candidate bins (e.g. 150 BPM sits
    // between i=34 and i=35), so a ±2-bin window captures that spread.
    // Uniform distribution → 5/N mass → conf ≈ 0. Sharp unimodal → conf ≈ 1.
    float peak_mass = 0.0f;
    for (int j = best_idx - 2; j <= best_idx + 2; j++) {
        if (j >= 0 && j < static_cast<int>(kTempoCandidates)) peak_mass += delta_[j];
    }
    float uniform_mass = 5.0f / static_cast<float>(kTempoCandidates);
    float conf = (peak_mass - uniform_mass) / (1.0f - uniform_mass);
    current_confidence_ = std::min(1.0f, std::max(0.0f, conf));
}

inline void BTrack::make_log_gaussian_weights_(float *w, uint16_t n, float beat_period) {
    // Reference createLogGaussianTransitionWeighting(): v starts at -2*beatPeriod,
    // weight = exp(-0.5 * (tightness * log(-v/beatPeriod))^2). The window runs
    // from 2 beat-periods in the past to ~0.5 beat-period in the past.
    if (beat_period < 1.0f) beat_period = 1.0f;
    float v = -2.0f * beat_period;
    for (uint16_t i = 0; i < n; i++) {
        float ratio = -v / beat_period;
        if (ratio <= 0.0f) ratio = 1e-6f;
        float a = kTightness * logf(ratio);
        w[i] = expf(-0.5f * a * a);
        v += 1.0f;
    }
}

inline float BTrack::cumulative_score_value_(const float *cs_linear, uint16_t buf_size,
                                             const float *log_gauss, int32_t start_idx,
                                             int32_t end_idx, float onset, float alpha) {
    float max_val = 0.0f;
    int32_t n = 0;
    for (int32_t i = start_idx; i <= end_idx; i++) {
        if (i >= 0 && i < static_cast<int32_t>(buf_size)) {
            float v = cs_linear[i] * log_gauss[n];
            if (v > max_val) max_val = v;
        }
        n++;
    }
    return (1.0f - alpha) * onset + alpha * max_val;
}

// Update cumulative-score ring — called every frame (cheap: short window scan).
// This is the core DP step in BTrack: for each frame, compute a score as a blend
// of the current onset and the best past score weighted by a log-Gaussian
// centred where a beat would be expected ~1 beat ago.
inline void BTrack::update_cumulative_score_(float onset) {
    float bp = beat_period_frames_val_;
    if (bp < 2.0f) bp = 2.0f;

    // Window into the past: [oldest+N - 2*bp, oldest+N - bp/2]
    // We work in "linear time" order (oldest at 0, newest at N-1).
    // cs_linear is our ring with oldest = cumulative_score_[history_write_] (since
    // we store cumulative_score_ in a matching ring — same write index as onset).
    int32_t N = kHistoryLen;
    int32_t win_end = N - static_cast<int32_t>(roundf(bp * 0.5f));
    int32_t win_start = N - static_cast<int32_t>(roundf(2.0f * bp));
    if (win_start < 0) win_start = 0;
    if (win_end >= N) win_end = N - 1;
    int32_t win_size = win_end - win_start + 1;
    if (win_size < 2) {
        // Degenerate — just store the onset.
        cumulative_score_[history_write_] = onset;
        return;
    }

    // Linearise the past cumulative_score_ ring into a small scratch covering the
    // window. history_write_ is the slot we'll write into THIS frame (one past
    // newest); the oldest slot is history_write_ itself (since it wraps).
    // To avoid large scratch, index the ring directly.
    //
    // Uses the class-member log_gauss_ (sized kLogGaussMaxLen=256, more than
    // enough — win_size = ~1.5 × beat_period_frames_val_ is at most ~96 at
    // 80 BPM and far less at higher tempos). Was previously a 2 KB stack-local
    // array which contributed to FFT-task stack pressure.
    if (static_cast<uint16_t>(win_size) > kLogGaussMaxLen) {
        // Defensive cap: if some pathological beat period somehow exceeded
        // the scratch budget we'd corrupt memory. Truncate instead.
        win_size = kLogGaussMaxLen;
    }
    make_log_gaussian_weights_(log_gauss_, static_cast<uint16_t>(win_size), bp);

    // Compute max over window of cumulative_score[i] * log_gauss[n].
    float max_val = 0.0f;
    for (int32_t k = 0; k < win_size; k++) {
        int32_t lin = win_start + k;
        // linear index 0 == history_write_ slot (oldest after we overwrite it)
        uint16_t ring = static_cast<uint16_t>((history_write_ + lin) % kHistoryLen);
        float v = cumulative_score_[ring] * log_gauss_[k];
        if (v > max_val) max_val = v;
    }
    float new_score = (1.0f - kAlpha) * onset + kAlpha * max_val;
    cumulative_score_[history_write_] = new_score;
}

// Predict the next beat using DP over an extrapolated cumulative-score window.
// Ported from reference predictBeat(). We look ~0.5 beat-periods into the future
// and find the peak of (future_cumulative_score * beat_expectation_window),
// then set time_to_next_beat_ to that offset in frames.
inline void BTrack::predict_beat_() {
    float bp = beat_period_frames_val_;
    if (bp < 2.0f) bp = 2.0f;
    int16_t expectation_window = static_cast<int16_t>(bp);
    if (expectation_window < 2) expectation_window = 2;
    if (expectation_window > static_cast<int16_t>(kHistoryLen)) {
        expectation_window = kHistoryLen;
    }

    // future_cs_ (class member): first kHistoryLen = current cumulative_score
    // in linear form, then next `expectation_window` samples = synthesised future.
    // Class member (not stack local) — the FFT task stack is only 6144 bytes.

    // Linearise cumulative_score into time order (oldest→newest).
    for (uint16_t i = 0; i < kHistoryLen; i++) {
        future_cs_[i] = cumulative_score_[(history_write_ + i) % kHistoryLen];
    }

    int32_t N = kHistoryLen;
    int32_t start_idx = N - static_cast<int32_t>(roundf(2.0f * bp));
    int32_t end_idx = N - static_cast<int32_t>(roundf(bp * 0.5f));
    if (start_idx < 0) start_idx = 0;
    int32_t past_window_size = end_idx - start_idx + 1;
    if (past_window_size < 2) {
        time_to_next_prediction_ = static_cast<int16_t>(bp * 0.5f);
        return;
    }

    // Log-gaussian weighting window for cumulative-score synthesis.
    // Use class-member buffer; guard against exceeding its static size.
    uint16_t pw_clamped = (past_window_size > static_cast<int32_t>(kLogGaussMaxLen))
                              ? kLogGaussMaxLen
                              : static_cast<uint16_t>(past_window_size);
    make_log_gaussian_weights_(log_gauss_, pw_clamped, bp);

    // Synthesise future cumulative score (alpha=1.0, onset=0 as in reference).
    for (int16_t i = 0; i < expectation_window; i++) {
        int32_t s = start_idx + i;
        int32_t e = end_idx + i;
        float max_val = 0.0f;
        int32_t n = 0;
        for (int32_t j = s; j <= e && n < pw_clamped; j++) {
            if (j >= 0 && j < static_cast<int32_t>(N + expectation_window)) {
                float v = future_cs_[j] * log_gauss_[n];
                if (v > max_val) max_val = v;
            }
            n++;
        }
        future_cs_[N + i] = max_val;   // alpha=1, onset=0
    }

    // Beat-expectation window: Gaussian centred at bp/2 frames into the future.
    float best_val = 0.0f;
    int16_t best_n = static_cast<int16_t>(bp * 0.5f);  // fallback
    float half_bp = bp * 0.5f;
    for (int16_t n = 0; n < expectation_window; n++) {
        float v = static_cast<float>(n + 1);
        float g = expf(-0.5f * ((v - half_bp) * (v - half_bp)) / (half_bp * half_bp));
        float w = future_cs_[N + n] * g;
        if (w > best_val) {
            best_val = w;
            best_n = n;
        }
    }
    // Reference sets timeToNextBeat to best_n, then timeToNextPrediction =
    // timeToNextBeat + bp/2. We do the same; timers count down per frame.
    time_to_next_beat_ = best_n;
    time_to_next_prediction_ = best_n + static_cast<int16_t>(roundf(half_bp));
    if (time_to_next_prediction_ < 1) time_to_next_prediction_ = 1;
}

}  // namespace audio_reactive
}  // namespace esphome
