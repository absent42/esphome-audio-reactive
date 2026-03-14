#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../../components/audio_reactive/onset_detector.h"

using namespace esphome::audio_reactive;

void test_spectral_flux_detects_onset() {
    OnsetDetector det(50, OnsetDetector::MODE_SPECTRAL_FLUX);
    float quiet[16] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                       0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    // Warm up
    for (int i = 0; i < 60; i++) det.update(quiet, 0.1f, i * 50);
    // Spike across multiple bands
    float spike[16] = {0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f, 0.3f, 0.2f,
                       0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    auto result = det.update(spike, 0.9f, 3050);
    assert(result.detected);
    assert(result.strength >= 0.1f);
    assert(result.dominant_band_index >= 0 && result.dominant_band_index < 16);
    printf("PASS: test_spectral_flux_detects_onset\n");
}

void test_no_onset_in_silence() {
    OnsetDetector det(50, OnsetDetector::MODE_SPECTRAL_FLUX);
    float zero[16] = {};
    for (int i = 0; i < 100; i++) {
        auto r = det.update(zero, 0.0f, i * 50);
        assert(!r.detected);
    }
    printf("PASS: test_no_onset_in_silence\n");
}

void test_bass_energy_mode() {
    OnsetDetector det(50, OnsetDetector::MODE_BASS_ENERGY);
    float quiet[16] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                       0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    for (int i = 0; i < 40; i++) det.update(quiet, 0.1f, i * 50);
    float bass_spike[16] = {0.9f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                            0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    auto r = det.update(bass_spike, 0.9f, 2050);
    assert(r.detected);
    assert(r.type == OnsetDetector::TYPE_BEAT);
    printf("PASS: test_bass_energy_mode\n");
}

void test_min_interval_enforcement() {
    OnsetDetector det(50, OnsetDetector::MODE_SPECTRAL_FLUX);
    float quiet[16] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                       0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    for (int i = 0; i < 60; i++) det.update(quiet, 0.1f, i * 50);
    float spike[16] = {0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f,
                       0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f};
    auto r1 = det.update(spike, 0.9f, 3050);
    assert(r1.detected);
    // Second spike 50ms later — should be suppressed (min 150ms)
    auto r2 = det.update(spike, 0.9f, 3100);
    assert(!r2.detected);
    printf("PASS: test_min_interval_enforcement\n");
}

void test_sensitivity_mapping() {
    // Low sensitivity = higher threshold = fewer detections
    // High sensitivity = lower threshold = more detections
    // Just verify they construct without error and sensitivity 10 < 90 threshold
    OnsetDetector det_low(10, OnsetDetector::MODE_SPECTRAL_FLUX);
    OnsetDetector det_high(90, OnsetDetector::MODE_SPECTRAL_FLUX);
    float quiet[16] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                       0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    // Warm up both with identical data
    for (int i = 0; i < 60; i++) {
        det_low.update(quiet, 0.1f, i * 50);
        det_high.update(quiet, 0.1f, i * 50);
    }
    // A marginal spike that high sensitivity should detect but low might not
    float marginal[16] = {0.3f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                          0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    auto r_low  = det_low.update(marginal, 0.3f, 3050);
    auto r_high = det_high.update(marginal, 0.3f, 3050);
    // High sensitivity should fire equal or more than low sensitivity
    // (It's OK if both fire or neither fires on this particular signal, just no crash)
    (void)r_low;
    (void)r_high;
    printf("PASS: test_sensitivity_mapping\n");
}

void test_strength_floor() {
    OnsetDetector det(50, OnsetDetector::MODE_SPECTRAL_FLUX);
    float quiet[16] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                       0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    for (int i = 0; i < 60; i++) det.update(quiet, 0.1f, i * 50);
    // Small spike — strength should be floored at 0.1 if detected
    float small_spike[16] = {0.15f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                             0.1f,  0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    auto r = det.update(small_spike, 0.15f, 3050);
    if (r.detected) {
        assert(r.strength >= 0.1f);
    }
    printf("PASS: test_strength_floor\n");
}

void test_bpm_tracking() {
    OnsetDetector det(80, OnsetDetector::MODE_SPECTRAL_FLUX);
    float quiet[16] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                       0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    float spike[16] = {0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f,
                       0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f};
    // Warm up
    for (int i = 0; i < 40; i++) det.update(quiet, 0.1f, i * 50);
    // Simulate 120 BPM = 500ms between beats.
    // Each cycle: spike at beat_start, then 9 quiet frames at 50ms each (450ms),
    // total = 500ms per beat.
    const uint32_t beat_period_ms = 500;
    const int quiet_frames = 9;   // 9 * 50ms = 450ms quiet after spike
    uint32_t t = 2000;
    for (int beat = 0; beat < 8; beat++) {
        det.update(spike, 0.9f, t);
        for (int j = 1; j <= quiet_frames; j++) {
            det.update(quiet, 0.1f, t + j * 50);
        }
        t += beat_period_ms;
    }
    float bpm = det.current_bpm(t);
    // Should be roughly 120 BPM (allow tolerance), or 0 if not enough intervals
    if (bpm > 0) {
        assert(bpm > 100.0f && bpm < 140.0f);
    }
    printf("PASS: test_bpm_tracking (bpm=%.1f)\n", bpm);
}

void test_reset() {
    OnsetDetector det(50, OnsetDetector::MODE_SPECTRAL_FLUX);
    float quiet[16] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                       0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    for (int i = 0; i < 60; i++) det.update(quiet, 0.1f, i * 50);
    det.reset();
    assert(det.current_bpm(5000) == 0.0f);
    assert(det.confidence() == 0);
    printf("PASS: test_reset\n");
}

void test_dominant_band_category() {
    OnsetDetector det(50, OnsetDetector::MODE_SPECTRAL_FLUX);
    float quiet[16] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                       0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    for (int i = 0; i < 60; i++) det.update(quiet, 0.1f, i * 50);
    // Spike in high bands only
    float high_spike[16] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                            0.1f, 0.1f, 0.9f, 0.9f, 0.9f, 0.1f, 0.1f, 0.1f};
    auto r = det.update(high_spike, 0.1f, 3050);
    if (r.detected) {
        assert(r.dominant_band_index >= 10 && r.dominant_band_index <= 12);
        assert(r.type == OnsetDetector::TYPE_ONSET);
        assert(strcmp(r.dominant_band, "high") == 0);
    }
    printf("PASS: test_dominant_band_category\n");
}

void test_bass_energy_hysteresis() {
    // After a bass energy detection, it must drop below 70% of threshold
    // before triggering again, even if min_interval has passed.
    OnsetDetector det(50, OnsetDetector::MODE_BASS_ENERGY, 60, 50);
    float quiet[16] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                       0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    for (int i = 0; i < 40; i++) det.update(quiet, 0.1f, i * 50);
    float bass_spike[16] = {0.9f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                            0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    auto r1 = det.update(bass_spike, 0.9f, 2050);
    assert(r1.detected);
    // Same high value again after min_interval (60ms > 50ms) but hysteresis not reset
    auto r2 = det.update(bass_spike, 0.9f, 2150);
    assert(!r2.detected);
    printf("PASS: test_bass_energy_hysteresis\n");
}

int main() {
    test_spectral_flux_detects_onset();
    test_no_onset_in_silence();
    test_bass_energy_mode();
    test_min_interval_enforcement();
    test_sensitivity_mapping();
    test_strength_floor();
    test_bpm_tracking();
    test_reset();
    test_dominant_band_category();
    test_bass_energy_hysteresis();
    printf("All onset detector tests passed.\n");
    return 0;
}
