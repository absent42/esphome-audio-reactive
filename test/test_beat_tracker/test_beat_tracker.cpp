#include <cassert>
#include <cstdio>

#include "../../components/audio_reactive/beat_tracker.h"

using namespace esphome::audio_reactive;

void test_no_bpm_initially() {
    BeatTracker tracker(20.0f);
    auto r = tracker.result();
    assert(r.bpm == 0.0f);
    assert(r.confidence == 0.0f);
    printf("PASS: test_no_bpm_initially\n");
}

void test_120bpm_detection() {
    // 20Hz update rate, 120 BPM = 2 beats/s = 1 beat every 10 frames
    BeatTracker tracker(20.0f);
    const int period = 10;
    for (int i = 0; i < 200; i++) {
        float val = (i % period == 0) ? 1.0f : 0.05f;
        tracker.process(val);
    }
    auto r = tracker.result();
    printf("  120bpm test: bpm=%.1f confidence=%.3f\n", r.bpm, r.confidence);
    assert(r.bpm >= 100.0f && r.bpm <= 140.0f);
    printf("PASS: test_120bpm_detection\n");
}

void test_90bpm_detection() {
    // 20Hz update rate, 90 BPM = 1.5 beats/s = 1 beat every ~13.3 frames
    // Use period=13 frames → BPM = 20*60/13 ≈ 92.3 BPM
    BeatTracker tracker(20.0f);
    const int period = 13;
    for (int i = 0; i < 200; i++) {
        float val = (i % period == 0) ? 1.0f : 0.05f;
        tracker.process(val);
    }
    auto r = tracker.result();
    printf("  90bpm test: bpm=%.1f confidence=%.3f\n", r.bpm, r.confidence);
    assert(r.bpm >= 75.0f && r.bpm <= 105.0f);
    printf("PASS: test_90bpm_detection\n");
}

void test_silence_no_bpm() {
    BeatTracker tracker(20.0f);
    for (int i = 0; i < 200; i++) {
        tracker.process(0.0f);
    }
    auto r = tracker.result();
    printf("  silence test: bpm=%.1f confidence=%.3f\n", r.bpm, r.confidence);
    assert(r.bpm == 0.0f || r.confidence < 0.1f);
    printf("PASS: test_silence_no_bpm\n");
}

void test_beat_phase_range() {
    BeatTracker tracker(20.0f);
    const int period = 10;
    for (int i = 0; i < 200; i++) {
        float val = (i % period == 0) ? 1.0f : 0.05f;
        tracker.process(val);
        auto r = tracker.result();
        assert(r.phase >= 0.0f && r.phase <= 1.0f);
    }
    printf("PASS: test_beat_phase_range\n");
}

void test_reset() {
    BeatTracker tracker(20.0f);
    const int period = 10;
    for (int i = 0; i < 200; i++) {
        float val = (i % period == 0) ? 1.0f : 0.05f;
        tracker.process(val);
    }
    tracker.reset();
    auto r = tracker.result();
    assert(r.bpm == 0.0f);
    assert(r.confidence == 0.0f);
    printf("PASS: test_reset\n");
}

void test_tempo_doubling_protection() {
    // 20Hz, onset every 20 frames → 60 BPM.
    // Without protection might report 120 BPM (doubling). Should stay in 45-80 range.
    BeatTracker tracker(20.0f);
    const int period = 20;
    for (int i = 0; i < 200; i++) {
        float val = (i % period == 0) ? 1.0f : 0.05f;
        tracker.process(val);
    }
    auto r = tracker.result();
    printf("  tempo_doubling test: bpm=%.1f confidence=%.3f\n", r.bpm, r.confidence);
    // Should report the actual tempo (45-80), not the doubled version (>100)
    if (r.bpm > 0.0f) {
        assert(r.bpm >= 45.0f && r.bpm <= 80.0f);
    }
    printf("PASS: test_tempo_doubling_protection\n");
}

int main() {
    test_no_bpm_initially();
    test_120bpm_detection();
    test_90bpm_detection();
    test_silence_no_bpm();
    test_beat_phase_range();
    test_reset();
    test_tempo_doubling_protection();
    printf("All beat tracker tests passed.\n");
    return 0;
}
