// test/test_btrack/test_btrack.cpp
//
// Unit tests for the BTrack beat tracker. Tempo induction is delegated to
// TempoEstimator (see test/test_tempo_estimator for its per-helper coverage);
// the tests here cover the cumulative-score DP, beat prediction, and the
// full pipeline on metronome, club-pattern, and speech-noise fixtures.
//
// Build (native):  g++ -std=c++17 -O2 -I components/audio_reactive \
//                      test/test_btrack/test_btrack.cpp -o /tmp/test_btrack
//                  (PlatformIO `pio test -e native -f test_btrack` does this in CI)

#include <cassert>
#include <cmath>
#include <cstdio>

#include "../../components/audio_reactive/btrack.h"
#include "../test_helpers_rhythm.h"

using namespace esphome::audio_reactive;

// ---------------------------------------------------------------------------
// log-Gaussian weights / cumulative score DP / predict_beat
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
// Tempo induction is not exercised here — we override beat_period via
// debug_set_beat_period_frames() so the DP path runs in isolation.

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
        // Re-assert beat period each frame so the tempo-side update doesn't
        // perturb the test.
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
// Full pipeline integration (tempo induction wired in).
// ---------------------------------------------------------------------------
// With the tempo path active, BTrack must derive the beat period from the
// input rather than from debug_set_beat_period_frames(). Tests at three BPMs
// — 90, 120, 150 — to confirm the lock isn't biased toward the 120 BPM prior.

static float feed_metronome_get_final_bpm(float bpm, float seconds,
                                          float *conf_out = nullptr) {
    BTrack bt;
    bt.reset();
    const float period = BTrack::kFrameHz * 60.0f / bpm;
    const int total_frames = static_cast<int>(BTrack::kFrameHz * seconds);
    // Accumulate the pulse position in float so the AVERAGE period is exact.
    // (The old `next_pulse += (int)period` truncated: a "150 BPM" call
    // actually produced a 152.0 BPM train — period 34.45 -> 34 — so a
    // correct estimator would rightly fail a tight assertion.)
    float next_pulse = period;
    BTrack::Result last{};
    for (int f = 0; f < total_frames; f++) {
        const float onset = (f == static_cast<int>(next_pulse)) ? 10.0f : 0.0f;
        if (f == static_cast<int>(next_pulse)) next_pulse += period;
        last = bt.process(onset);
    }
    if (conf_out) *conf_out = last.confidence;
    return last.bpm;
}

void test_locks_on_120_bpm_via_full_pipeline() {
    const float bpm = feed_metronome_get_final_bpm(120.0f, 20.0f);
    if (fabsf(bpm - 120.0f) > 2.5f) {
        fprintf(stderr,
                "FAIL: 120 BPM pulse train → BTrack reports %.1f BPM (want 120 +-2.5)\n",
                bpm);
        assert(false);
    }
    printf("PASS: test_locks_on_120_bpm_via_full_pipeline (bpm=%.1f)\n", bpm);
}

void test_locks_on_90_bpm_via_full_pipeline() {
    // 90 BPM is well off the 120-BPM prior center. The old port locked at
    // 114-120 BPM regardless of input on real music; a working pipeline must
    // lock at 90 +-2.5 here.
    const float bpm = feed_metronome_get_final_bpm(90.0f, 20.0f);
    if (fabsf(bpm - 90.0f) > 2.5f) {
        fprintf(stderr,
                "FAIL: 90 BPM pulse train → BTrack reports %.1f BPM (want 90 +-2.5)\n",
                bpm);
        assert(false);
    }
    printf("PASS: test_locks_on_90_bpm_via_full_pipeline (bpm=%.1f)\n", bpm);
}

void test_locks_on_150_bpm_via_full_pipeline() {
    const float bpm = feed_metronome_get_final_bpm(150.0f, 20.0f);
    if (fabsf(bpm - 150.0f) > 2.5f) {
        fprintf(stderr,
                "FAIL: 150 BPM pulse train → BTrack reports %.1f BPM (want 150 +-2.5)\n",
                bpm);
        assert(false);
    }
    printf("PASS: test_locks_on_150_bpm_via_full_pipeline (bpm=%.1f)\n", bpm);
}

// ---------------------------------------------------------------------------
// Warmup + realistic-rhythm regression tests (club patterns, tempo change,
// speech noise). These fixtures expose the 114/150 attractor bugs that pure
// metronome trains cannot; see docs/plans/2026-07-02-tempo-induction-
// rewrite.md, Background.
// ---------------------------------------------------------------------------

static float feed_env_get_final_bpm(const float *env, int n, float *conf_out) {
    BTrack bt;
    bt.reset();
    BTrack::Result r{};
    for (int f = 0; f < n; f++) r = bt.process(env[f]);
    if (conf_out) *conf_out = r.confidence;
    return r.bpm;
}

void test_warmup_suppresses_confidence() {
    static float env[4096];
    rhythm_fixtures::seed(42);
    int n = rhythm_fixtures::build_club(128.0f, 10.0f, BTrack::kFrameHz, env, 4096);
    BTrack bt;
    bt.reset();
    for (int f = 0; f < n; f++) {
        auto r = bt.process(env[f]);
        if (f < BTrack::kWarmupFrames) {
            if (r.confidence != 0.0f) {
                fprintf(stderr, "FAIL: confidence %.2f at warmup frame %d\n",
                        r.confidence, f);
                assert(false);
            }
        }
    }
    printf("PASS: test_warmup_suppresses_confidence\n");
}

void test_club_115_and_152_do_not_alias() {
    static float env[8192];
    struct Case { float truth; } cases[] = {{115}, {152}};
    const uint32_t seeds[] = {42, 555, 90210};
    for (auto &c : cases) {
        for (uint32_t s : seeds) {
            rhythm_fixtures::seed(s);
            int n = rhythm_fixtures::build_club(c.truth, 30.0f, BTrack::kFrameHz,
                                                env, 8192);
            float conf = 0.0f;
            float bpm = feed_env_get_final_bpm(env, n, &conf);
            if (std::fabs(bpm - c.truth) > 2.5f) {
                fprintf(stderr, "FAIL: club %.0f seed %u -> %.1f BPM (want +-2.5)\n",
                        c.truth, s, bpm);
                assert(false);
            }
        }
    }
    printf("PASS: test_club_115_and_152_do_not_alias\n");
}

// DOCUMENTED LIMITATION - sub-harmonic locks above ~160 BPM with strong
// eighth-note content (full-pipeline mirror of the TempoEstimator test of
// the same name; see test_tempo_estimator.cpp for the mechanism and the
// reverted half-lag experiment). Measured: 168 -> 112.0-112.1 at conf 0.33
// (3:2 alias, above the 0.3 publish gate), 176 -> 88.0 / 117.3 at conf
// 0.00-0.34 depending on seed. This pins the failure to a harmonic fraction
// of the truth with marginal confidence so any drift is caught.
void test_club_high_tempo_subharmonic_documented_limitation() {
    static float env[8192];
    struct Case { float truth; } cases[] = {{168}, {176}};
    const uint32_t seeds[] = {42, 555, 90210};
    for (auto &c : cases) {
        for (uint32_t s : seeds) {
            rhythm_fixtures::seed(s);
            int n = rhythm_fixtures::build_club(c.truth, 30.0f, BTrack::kFrameHz,
                                                env, 8192);
            float conf = 0.0f;
            float bpm = feed_env_get_final_bpm(env, n, &conf);
            const bool correct  = std::fabs(bpm - c.truth) <= 2.5f;
            const bool alias_23 = std::fabs(bpm - c.truth * (2.0f / 3.0f)) <= 2.5f;
            const bool alias_12 = std::fabs(bpm - c.truth * 0.5f) <= 2.5f;
            if (!(correct || alias_23 || alias_12)) {
                fprintf(stderr,
                        "FAIL: club %.0f seed %u -> %.1f BPM (neither truth nor "
                        "a documented 2:3 / 1:2 sub-harmonic)\n", c.truth, s, bpm);
                assert(false);
            }
            // Club-alias confidence bound; the clean-metronome edge case
            // below exceeds it (0.60) and gets its own bound.
            if (!correct && !(conf <= 0.40f)) {
                fprintf(stderr, "FAIL: club %.0f seed %u aliased to %.1f with "
                        "conf %.2f (> 0.40)\n", c.truth, s, bpm, conf);
                assert(false);
            }
        }
    }
    // The high-tempo limitation is broader than eighth-heavy club material:
    // a CLEAN 180 BPM metronome (no eighth-note content) also locks the 1:2
    // sub-harmonic - measured 90.0 at conf 0.60.
    {
        float conf = 0.0f;
        float bpm = feed_metronome_get_final_bpm(180.0f, 30.0f, &conf);
        const bool correct  = std::fabs(bpm - 180.0f) <= 2.5f;
        const bool alias_12 = std::fabs(bpm - 90.0f) <= 2.5f;
        if (!(correct || alias_12)) {
            fprintf(stderr, "FAIL: metronome 180 -> %.1f BPM (neither truth "
                    "nor the documented 1:2 sub-harmonic)\n", bpm);
            assert(false);
        }
        if (!correct && !(conf <= 0.65f)) {
            fprintf(stderr, "FAIL: metronome 180 aliased to %.1f with conf "
                    "%.2f (> 0.65)\n", bpm, conf);
            assert(false);
        }
    }
    printf("PASS: test_club_high_tempo_subharmonic_documented_limitation\n");
}

// DOCUMENTED LIMITATION - octave (2:1) locks below ~85 BPM with eighth-note
// content (full-pipeline mirror of the TempoEstimator test of the same
// name): the hats form a genuine pulse at twice the beat rate and the
// 120-centred prior prefers the double, so slow club material reports
// double tempo - and, unlike the high-tempo aliases, it publishes
// CONFIDENTLY. Measured (seeds 42/555/90210): 60 -> 120.0 at conf
// 0.88-0.89, 65 -> 130.0 at conf 0.80-0.84, 70 -> 140.0 at conf 0.81-0.87,
// 75 -> 150.0 at conf 0.68-0.73 (80 also doubles at conf 0.61-0.68; 85 is
// the first correct lock, at conf 0.33-0.34, barely above the 0.3 gate).
void test_club_low_tempo_octave_documented_limitation() {
    static float env[8192];
    struct Case { float truth; } cases[] = {{60}, {65}, {70}, {75}};
    const uint32_t seeds[] = {42, 555, 90210};
    for (auto &c : cases) {
        for (uint32_t s : seeds) {
            rhythm_fixtures::seed(s);
            int n = rhythm_fixtures::build_club(c.truth, 30.0f, BTrack::kFrameHz,
                                                env, 8192);
            float conf = 0.0f;
            float bpm = feed_env_get_final_bpm(env, n, &conf);
            const bool correct = std::fabs(bpm - c.truth) <= 2.5f;
            const bool dbl     = std::fabs(bpm - 2.0f * c.truth) <= 2.5f;
            if (!(correct || dbl)) {
                fprintf(stderr,
                        "FAIL: club %.0f seed %u -> %.1f BPM (neither truth "
                        "nor the documented 2:1 double)\n", c.truth, s, bpm);
                assert(false);
            }
        }
    }
    printf("PASS: test_club_low_tempo_octave_documented_limitation\n");
}

void test_tempo_change_relocks_within_15s() {
    static float env[16384];
    rhythm_fixtures::seed(7);
    int n1 = rhythm_fixtures::build_club(150.0f, 30.0f, BTrack::kFrameHz, env, 16384);
    int n2 = rhythm_fixtures::build_club(122.0f, 30.0f, BTrack::kFrameHz,
                                         env + n1, 16384 - n1);
    BTrack bt;
    bt.reset();
    for (int f = 0; f < n1; f++) bt.process(env[f]);
    int relock_frame = -1, held = 0;
    for (int f = 0; f < n2; f++) {
        auto r = bt.process(env[n1 + f]);
        if (std::fabs(r.bpm - 122.0f) <= 2.5f) {
            held++;
            if (held >= (int)(3 * BTrack::kFrameHz) && relock_frame < 0)
                relock_frame = f - held + 1;
        } else {
            held = 0;
        }
    }
    if (relock_frame < 0 || relock_frame > (int)(15 * BTrack::kFrameHz)) {
        fprintf(stderr, "FAIL: relock frame %d (want <= 15 s)\n", relock_frame);
        assert(false);
    }
    printf("PASS: test_tempo_change_relocks_within_15s (%.1f s)\n",
           relock_frame / BTrack::kFrameHz);
}

void test_speech_noise_publishes_zero() {
    static float env[8192];
    rhythm_fixtures::seed(555);
    int n = rhythm_fixtures::build_speech(60.0f, BTrack::kFrameHz, env, 8192);
    BTrack bt;
    bt.reset();
    int confident = 0, total = 0;
    for (int f = 0; f < n; f++) {
        auto r = bt.process(env[f]);
        if (f < BTrack::kWarmupFrames) continue;
        total++;
        if (r.confidence >= BTrack::kSilenceConfidence) confident++;
    }
    if (confident * 10 > total) {
        fprintf(stderr, "FAIL: speech confident on %d/%d frames\n", confident, total);
        assert(false);
    }
    printf("PASS: test_speech_noise_publishes_zero (%d/%d)\n", confident, total);
}

int main() {
    test_log_gaussian_weights_peak_at_beat_period();
    test_periodic_input_produces_beats_at_expected_cadence();
    test_locks_on_120_bpm_via_full_pipeline();
    test_locks_on_90_bpm_via_full_pipeline();
    test_locks_on_150_bpm_via_full_pipeline();
    test_warmup_suppresses_confidence();
    test_club_115_and_152_do_not_alias();
    test_club_high_tempo_subharmonic_documented_limitation();
    test_club_low_tempo_octave_documented_limitation();
    test_tempo_change_relocks_within_15s();
    test_speech_noise_publishes_zero();
    printf("ALL BTRACK UNIT TESTS PASSED\n");
    return 0;
}
