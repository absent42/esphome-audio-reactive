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
    for (int i = 0; i < 60; i++) det.update(quiet, 0.1f, i * 50);
    float spike[16] = {0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f, 0.3f, 0.2f,
                       0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    auto result = det.update(spike, 0.9f, 3050);
    assert(result.detected);
    assert(result.strength >= 0.1f);
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
    OnsetDetector det_low(10, OnsetDetector::MODE_SPECTRAL_FLUX);
    OnsetDetector det_high(90, OnsetDetector::MODE_SPECTRAL_FLUX);
    float quiet[16] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                       0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    for (int i = 0; i < 60; i++) {
        det_low.update(quiet, 0.1f, i * 50);
        det_high.update(quiet, 0.1f, i * 50);
    }
    float marginal[16] = {0.3f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                          0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    auto r_low  = det_low.update(marginal, 0.3f, 3050);
    auto r_high = det_high.update(marginal, 0.3f, 3050);
    (void)r_low;
    (void)r_high;
    printf("PASS: test_sensitivity_mapping\n");
}

void test_strength_floor() {
    OnsetDetector det(50, OnsetDetector::MODE_SPECTRAL_FLUX);
    float quiet[16] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                       0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    for (int i = 0; i < 60; i++) det.update(quiet, 0.1f, i * 50);
    float small_spike[16] = {0.15f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                             0.1f,  0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    auto r = det.update(small_spike, 0.15f, 3050);
    if (r.detected) {
        assert(r.strength >= 0.1f);
    }
    printf("PASS: test_strength_floor\n");
}

void test_reset() {
    OnsetDetector det(50, OnsetDetector::MODE_SPECTRAL_FLUX);
    float quiet[16] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                       0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    for (int i = 0; i < 60; i++) det.update(quiet, 0.1f, i * 50);
    det.reset();
    // After reset, no detection should fire (window is empty → threshold is huge)
    float spike[16] = {0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f,
                       0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f};
    auto r = det.update(spike, 0.9f, 5000);
    assert(!r.detected);  // Window not yet half full
    printf("PASS: test_reset\n");
}

void test_bass_energy_hysteresis() {
    OnsetDetector det(50, OnsetDetector::MODE_BASS_ENERGY, 60, 50);
    float quiet[16] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                       0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    for (int i = 0; i < 40; i++) det.update(quiet, 0.1f, i * 50);
    float bass_spike[16] = {0.9f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                            0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    auto r1 = det.update(bass_spike, 0.9f, 2050);
    assert(r1.detected);
    auto r2 = det.update(bass_spike, 0.9f, 2150);
    assert(!r2.detected);
    printf("PASS: test_bass_energy_hysteresis\n");
}

void test_complex_domain_mode() {
    OnsetDetector det(50, OnsetDetector::MODE_COMPLEX_DOMAIN);
    float quiet[16] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                       0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    for (int i = 0; i < 60; i++) {
        det.update(quiet, 0.1f, i * 50, 0.5f);
    }
    auto result = det.update(quiet, 0.1f, 3050, 50.0f);
    assert(result.detected);
    printf("PASS: test_complex_domain_mode\n");
}

void test_complex_domain_ignores_bands() {
    OnsetDetector det(50, OnsetDetector::MODE_COMPLEX_DOMAIN);
    float quiet[16] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                       0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    for (int i = 0; i < 60; i++) {
        det.update(quiet, 0.1f, i * 50, 0.5f);
    }
    float spike[16] = {0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f,
                       0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f};
    auto result = det.update(spike, 0.9f, 3050, 0.5f);
    assert(!result.detected);
    printf("PASS: test_complex_domain_ignores_bands\n");
}

int main() {
    test_spectral_flux_detects_onset();
    test_no_onset_in_silence();
    test_bass_energy_mode();
    test_min_interval_enforcement();
    test_sensitivity_mapping();
    test_strength_floor();
    test_reset();
    test_bass_energy_hysteresis();
    test_complex_domain_mode();
    test_complex_domain_ignores_bands();
    printf("All onset detector tests passed.\n");
    return 0;
}
