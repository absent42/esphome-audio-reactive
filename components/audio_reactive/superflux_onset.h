#pragma once

// SuperFlux onset detector (Böck & Widmer, DAFx 2013).
// Consumes raw mel-band energies, computes log, max-filters the previous frame
// to suppress vibrato, computes spectral flux via half-wave-rectified difference,
// then applies adaptive peak-picking with a minimum inter-onset interval.

#include <cmath>
#include <cstdint>
#include <algorithm>

namespace esphome {
namespace audio_reactive {

template <uint8_t N_MEL>
class SuperFluxOnset {
 public:
    // Max-filter half-width (in mel bins). Spec recommends 3 for vibrato suppression.
    static constexpr uint8_t kMaxFilterMu = 3;
    // Minimum inter-onset interval (frames at ~86Hz). 30ms ~ 3 frames.
    static constexpr uint8_t kMinInterOnsetFrames = 3;
    // Peak-picker window sizes.
    static constexpr uint8_t kPeakPreMaxFrames = 3;   // frames before candidate for max check
    static constexpr uint8_t kPeakPostMaxFrames = 3;  // frames after
    static constexpr uint8_t kPeakPreAvgFrames = 10;  // frames before for mean threshold
    static constexpr uint8_t kPeakPostAvgFrames = 3;  // frames after
    // Delta threshold above the local mean (tunable).
    static constexpr float kDelta = 0.1f;
    // Ring-buffer length for the onset-strength history (must be >= pre+post windows).
    static constexpr uint8_t kHistoryLen = kPeakPreAvgFrames + kPeakPostAvgFrames + 2;

    struct Result {
        float strength;   // current-frame onset strength (always >= 0, unbounded)
        bool event;       // true on frame where an onset is confirmed
    };

    /// Process one mel frame; returns current onset strength and any confirmed event.
    /// mel_bands_raw: length N_MEL, raw (pre-log) energies from MelFilterbank.
    Result process(const float *mel_bands_raw) {
        constexpr float kEps = 1e-6f;
        // Log-compress current frame into log_curr_.
        for (uint8_t m = 0; m < N_MEL; m++) {
            log_curr_[m] = logf(mel_bands_raw[m] + kEps);
        }

        if (!has_prev_) {
            std::copy_n(log_curr_, N_MEL, log_prev_);
            has_prev_ = true;
            return {0.0f, false};
        }

        // Max-filter previous frame with radius kMaxFilterMu (vibrato suppression).
        // Compute flux = sum_{m} max(0, log_curr[m] - max(log_prev[m-mu .. m+mu])).
        float flux = 0.0f;
        for (uint8_t m = 0; m < N_MEL; m++) {
            float max_ref = -1e30f;
            int lo = static_cast<int>(m) - kMaxFilterMu;
            int hi = static_cast<int>(m) + kMaxFilterMu;
            if (lo < 0) lo = 0;
            if (hi >= N_MEL) hi = N_MEL - 1;
            for (int k = lo; k <= hi; k++) {
                if (log_prev_[k] > max_ref) max_ref = log_prev_[k];
            }
            float diff = log_curr_[m] - max_ref;
            if (diff > 0.0f) flux += diff;
        }

        std::copy_n(log_curr_, N_MEL, log_prev_);

        // Push flux into ring buffer.
        history_[write_idx_] = flux;
        write_idx_ = (write_idx_ + 1) % kHistoryLen;
        frames_since_onset_++;

        // Peak-pick on the CENTER candidate — the frame at history_[candidate_idx_].
        // Candidate is kPeakPostMaxFrames back from write_idx_.
        uint8_t candidate = (write_idx_ + kHistoryLen - kPeakPostMaxFrames - 1) % kHistoryLen;
        float candidate_val = history_[candidate];

        // Check candidate is local max within kPeakPreMaxFrames +/- kPeakPostMaxFrames.
        bool is_local_max = true;
        for (int off = -kPeakPreMaxFrames; off <= kPeakPostMaxFrames; off++) {
            if (off == 0) continue;
            uint8_t idx = (candidate + kHistoryLen + off) % kHistoryLen;
            if (history_[idx] >= candidate_val) {
                is_local_max = false;
                break;
            }
        }

        // Check candidate exceeds local mean + kDelta.
        bool is_above_mean = false;
        if (is_local_max) {
            float sum = 0.0f;
            uint8_t count = 0;
            for (int off = -kPeakPreAvgFrames; off <= kPeakPostAvgFrames; off++) {
                uint8_t idx = (candidate + kHistoryLen + off) % kHistoryLen;
                sum += history_[idx];
                count++;
            }
            float mean = (count > 0) ? sum / count : 0.0f;
            is_above_mean = candidate_val > (mean + kDelta);
        }

        // Warm-up gate: suppress events until the history ring holds enough real data
        // that the local-max and mean tests are meaningful.
        if (frames_seen_ < kHistoryLen) frames_seen_++;
        bool warm = frames_seen_ >= kHistoryLen;

        bool event = warm && is_local_max && is_above_mean && (frames_since_onset_ >= kMinInterOnsetFrames);
        if (event) frames_since_onset_ = 0;

        return {flux, event};
    }

    void reset() {
        has_prev_ = false;
        write_idx_ = 0;
        // Warm-up: suppress events until the history ring has filled with real data.
        // Otherwise the first non-zero flux value trivially exceeds the zero-padded
        // "mean" and fires a spurious onset on every unmute.
        frames_seen_ = 0;
        frames_since_onset_ = 0;  // no ready-to-fire preload
        for (uint8_t i = 0; i < kHistoryLen; i++) history_[i] = 0.0f;
    }

 protected:
    float log_curr_[N_MEL]{};
    float log_prev_[N_MEL]{};
    bool has_prev_{false};
    float history_[kHistoryLen]{};
    uint8_t write_idx_{0};
    uint8_t frames_since_onset_{0};
    uint8_t frames_seen_{0};  // warm-up counter (capped at kHistoryLen)
};

}  // namespace audio_reactive
}  // namespace esphome
