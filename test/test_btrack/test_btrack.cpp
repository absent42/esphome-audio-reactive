// test/test_btrack/test_btrack.cpp
#include <cassert>
#include <cmath>
#include <cstdio>

#define AUDIO_REACTIVE_NATIVE_TEST
#include "../../components/audio_reactive/btrack.h"

using namespace esphome::audio_reactive;

static void feed_metronome(BTrack &bt, float bpm, int seconds, BTrack::Result *last_out = nullptr) {
    float period = BTrack::kFrameHz * 60.0f / bpm;
    int total_frames = static_cast<int>(BTrack::kFrameHz * seconds);
    int next_beat_frame = static_cast<int>(period);
    for (int f = 0; f < total_frames; f++) {
        float onset = (f == next_beat_frame) ? 10.0f : 0.0f;
        if (f == next_beat_frame) next_beat_frame += static_cast<int>(period);
        auto r = bt.process(onset);
        if (last_out && f == total_frames - 1) *last_out = r;
    }
}

void test_locks_on_120_bpm() {
    BTrack bt;
    bt.reset();
    BTrack::Result r;
    feed_metronome(bt, 120.0f, 10, &r);
    assert(fabsf(r.bpm - 120.0f) < 5.0f);
    printf("PASS: test_locks_on_120_bpm (bpm=%.1f, conf=%.2f)\n", r.bpm, r.confidence);
}

// IMPORTANT: 120 BPM is the prior's peak. To distinguish "ACF found the right
// lag" from "prior happened to peak at 120", test non-120 BPMs too.
void test_locks_on_90_bpm() {
    BTrack bt;
    bt.reset();
    BTrack::Result r;
    feed_metronome(bt, 90.0f, 15, &r);  // 15s for slower tempo to lock
    assert(fabsf(r.bpm - 90.0f) < 5.0f);
    printf("PASS: test_locks_on_90_bpm (bpm=%.1f, conf=%.2f)\n", r.bpm, r.confidence);
}

void test_locks_on_150_bpm() {
    BTrack bt;
    bt.reset();
    BTrack::Result r;
    feed_metronome(bt, 150.0f, 10, &r);
    assert(fabsf(r.bpm - 150.0f) < 5.0f);
    printf("PASS: test_locks_on_150_bpm (bpm=%.1f, conf=%.2f)\n", r.bpm, r.confidence);
}

void test_cold_start_suppresses_events() {
    BTrack bt;
    bt.reset();
    int events_during_warmup = 0;
    float period = BTrack::kFrameHz * 60.0f / 120.0f;
    int warmup_frames = BTrack::kWarmupFrames;
    int next_beat_frame = static_cast<int>(period);
    for (int f = 0; f < warmup_frames; f++) {
        float onset = (f == next_beat_frame) ? 10.0f : 0.0f;
        if (f == next_beat_frame) next_beat_frame += static_cast<int>(period);
        auto r = bt.process(onset);
        if (r.beat_event) events_during_warmup++;
    }
    assert(events_during_warmup == 0);
    printf("PASS: test_cold_start_suppresses_events\n");
}

void test_silence_suppresses_events_eventually() {
    BTrack bt;
    bt.reset();
    // Lock onto 120 BPM.
    feed_metronome(bt, 120.0f, 10);
    // Then feed silence for ~4 seconds — events should stop.
    int events_during_silence = 0;
    int silence_frames = static_cast<int>(BTrack::kFrameHz * 4.0f);
    for (int f = 0; f < silence_frames; f++) {
        auto r = bt.process(0.0f);
        // Allow first 3 seconds of events (before silence confidence hold), count last ~1s.
        if (f > BTrack::kSilenceHoldFrames && r.beat_event) events_during_silence++;
    }
    assert(events_during_silence == 0);
    printf("PASS: test_silence_suppresses_events_eventually\n");
}

int main() {
    test_locks_on_120_bpm();
    test_locks_on_90_bpm();
    test_locks_on_150_bpm();
    test_cold_start_suppresses_events();
    test_silence_suppresses_events_eventually();
    printf("ALL BTRACK TESTS PASSED\n");
    return 0;
}
