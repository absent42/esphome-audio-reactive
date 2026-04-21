#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>

namespace esphome {
namespace audio_reactive {

/// BTrack-class beat tracker.
/// Input: onset-strength stream (86 Hz at 44.1 kHz / 2048-pt FFT / 512-hop).
/// Outputs per-frame: bpm, beat_phase [0,1], beat_confidence [0,1], beat_event.
class BTrack {
 public:
    static constexpr uint16_t kHistoryLen = 512;       // ~5.95s @ 86Hz
    static constexpr float   kFrameHz     = 86.13f;    // 44100 / 512
    static constexpr float   kBpmMin      = 60.0f;
    static constexpr float   kBpmMax      = 180.0f;
    static constexpr float   kBpmPriorCenter = 120.0f;
    static constexpr uint16_t kWarmupFrames = 256;     // ~3s before confidence ramps
    static constexpr uint16_t kMinLockFrames = 86;     // ~1s of confidence >= threshold for events
    static constexpr float   kLockConfidence = 0.4f;
    static constexpr float   kSilenceConfidence = 0.3f;
    static constexpr uint16_t kSilenceHoldFrames = 258;  // ~3s

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

    // Beat-phase accumulator (increments by 1/beat_period_frames each call)
    float beat_phase_{0.0f};
    uint16_t frames_above_lock_{0};
    uint16_t frames_below_lock_{0};
    // Count of consecutive near-zero onset frames — used to fast-drop confidence
    // during silence without having to wait for the onset ring buffer (~5.95s) to
    // fully flush. Threshold of kZeroOnsetEps mirrors SuperFlux's no-event floor.
    uint16_t zero_onset_streak_{0};
    static constexpr float kZeroOnsetEps = 1e-4f;

    // TODO Task 5.3: replace with full adamstark/BTrack DP algorithm (comb filterbank
    // tempo induction + dynamic-programming beat-phase tracking). This is a working
    // simplified ACF-plus-log-normal-prior version; the full DP port lands before merge.
    void update_tempo_estimate_();

    // Return period (frames/beat) for current_bpm_.
    float beat_period_frames_() const { return kFrameHz * 60.0f / current_bpm_; }
};

// Implementation — inlined to keep single-header.
inline void BTrack::reset() {
    for (uint16_t i = 0; i < kHistoryLen; i++) onset_history_[i] = 0.0f;
    history_write_ = 0;
    frames_since_reset_ = 0;
    current_bpm_ = kBpmPriorCenter;
    current_confidence_ = 0.0f;
    beat_phase_ = 0.0f;
    frames_above_lock_ = 0;
    frames_below_lock_ = 0;
    zero_onset_streak_ = 0;
}

inline BTrack::Result BTrack::process(float onset_strength) {
    // Push into ring buffer
    onset_history_[history_write_] = onset_strength;
    history_write_ = (history_write_ + 1) % kHistoryLen;
    frames_since_reset_++;

    // Track consecutive near-zero onset frames. The ACF-based confidence stays
    // high until the ring buffer flushes (~5.95s), so silence detection from
    // onset alone is needed to meet the kSilenceHoldFrames behavior.
    if (onset_strength < kZeroOnsetEps) {
        zero_onset_streak_++;
    } else {
        zero_onset_streak_ = 0;
    }

    // Re-estimate tempo every ~0.5s (43 frames) to amortize ACF cost.
    if (frames_since_reset_ % 43 == 0) {
        update_tempo_estimate_();
    }
    // Fast-path silence override: if the input has been silent for most of the
    // silence-hold window, drop confidence immediately so suppression kicks in.
    if (zero_onset_streak_ >= kSilenceHoldFrames) {
        current_confidence_ = 0.0f;
    }

    // Advance beat phase.
    float period = beat_period_frames_();
    beat_phase_ += 1.0f / period;
    bool event = false;
    if (beat_phase_ >= 1.0f) {
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

inline void BTrack::update_tempo_estimate_() {
    // Autocorrelation over onset_history_ for lags corresponding to kBpmMin..kBpmMax.
    // Apply log-normal prior centered at 120 BPM.
    // Simplified version; Task 5.3 replaces this with full adamstark/BTrack DP.
    const uint16_t lag_min = static_cast<uint16_t>(kFrameHz * 60.0f / kBpmMax);  // ~29 frames @ 180 BPM
    const uint16_t lag_max = static_cast<uint16_t>(kFrameHz * 60.0f / kBpmMin);  // ~86 frames @ 60 BPM

    float best_bpm = current_bpm_;
    float best_score = -1e30f;
    for (uint16_t lag = lag_min; lag <= lag_max; lag++) {
        float acf = 0.0f;
        for (uint16_t i = 0; i + lag < kHistoryLen; i++) {
            uint16_t idx_a = (history_write_ + kHistoryLen - 1 - i) % kHistoryLen;
            uint16_t idx_b = (history_write_ + kHistoryLen - 1 - i - lag) % kHistoryLen;
            acf += onset_history_[idx_a] * onset_history_[idx_b];
        }
        float bpm = kFrameHz * 60.0f / lag;
        // Log-normal prior (peaked at 120 BPM).
        float log_diff = logf(bpm / kBpmPriorCenter);
        float prior = expf(-0.5f * log_diff * log_diff / (0.3f * 0.3f));
        float score = acf * prior;
        if (score > best_score) {
            best_score = score;
            best_bpm = bpm;
        }
    }

    current_bpm_ = best_bpm;
    // Confidence: normalize best_score to [0,1] using a rough scaling.
    current_confidence_ = std::min(1.0f, std::max(0.0f, best_score / 50.0f));
}

}  // namespace audio_reactive
}  // namespace esphome
