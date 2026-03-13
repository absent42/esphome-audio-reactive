#include <cassert>
#include <cmath>
#include <cstdio>

#include "../components/audio_reactive/beat_detector.h"

using namespace esphome::audio_reactive;

void test_no_beat_during_warmup() {
    BeatDetector det(50, 20);
    for (int i = 0; i < 5; i++) {
        assert(!det.update(0.1f, 0));
    }
    printf("PASS: test_no_beat_during_warmup\n");
}

void test_beat_on_bass_spike() {
    BeatDetector det(50, 10);
    for (int i = 0; i < 15; i++) {
        det.update(0.1f, i * 50);
    }
    bool beat = det.update(0.9f, 800);
    assert(beat);
    printf("PASS: test_beat_on_bass_spike\n");
}

void test_no_beat_on_steady() {
    BeatDetector det(50, 10);
    bool any_beat = false;
    for (int i = 0; i < 30; i++) {
        if (det.update(0.5f, i * 50)) {
            any_beat = true;
        }
    }
    bool late_beat = false;
    for (int i = 30; i < 40; i++) {
        if (det.update(0.5f, i * 50)) {
            late_beat = true;
        }
    }
    assert(!late_beat);
    printf("PASS: test_no_beat_on_steady\n");
}

void test_min_interval_enforced() {
    BeatDetector det(50, 10, 150);
    for (int i = 0; i < 15; i++) {
        det.update(0.1f, i * 50);
    }
    // First spike → beat
    assert(det.update(0.9f, 750));
    // Second spike 50ms later → suppressed by min interval
    assert(!det.update(0.9f, 800));
    // Feed quiet samples to re-establish baseline before next spike
    for (int i = 0; i < 8; i++) {
        det.update(0.1f, 810 + i * 50);
    }
    // Spike well after min interval → beat
    assert(det.update(0.9f, 1300));
    printf("PASS: test_min_interval_enforced\n");
}

void test_sensitivity_affects_threshold() {
    BeatDetector det_low(20, 10);
    BeatDetector det_high(80, 10);

    for (int i = 0; i < 15; i++) {
        det_low.update(0.3f, i * 50);
        det_high.update(0.3f, i * 50);
    }

    bool low_beat = det_low.update(0.5f, 800);
    bool high_beat = det_high.update(0.5f, 800);
    assert(det_high.current_threshold() < det_low.current_threshold());
    printf("PASS: test_sensitivity_affects_threshold\n");
}

void test_bpm_tracking() {
    BeatDetector det(80, 10, 100);
    for (int i = 0; i < 15; i++) {
        det.update(0.1f, i * 50);
    }
    for (int beat = 0; beat < 10; beat++) {
        uint32_t t = 750 + beat * 500;
        det.update(0.9f, t);
        for (int j = 1; j < 10; j++) {
            det.update(0.1f, t + j * 50);
        }
    }
    float bpm = det.current_bpm();
    assert(bpm > 100.0f && bpm < 140.0f);
    printf("PASS: test_bpm_tracking (bpm=%.1f)\n", bpm);
}

int main() {
    test_no_beat_during_warmup();
    test_beat_on_bass_spike();
    test_no_beat_on_steady();
    test_min_interval_enforced();
    test_sensitivity_affects_threshold();
    test_bpm_tracking();
    printf("All beat detector tests passed.\n");
    return 0;
}
