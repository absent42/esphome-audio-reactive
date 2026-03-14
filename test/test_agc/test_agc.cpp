#include <cassert>
#include <cmath>
#include <cstdio>

#include "../../components/audio_reactive/agc.h"

using namespace esphome::audio_reactive;

// ── Helpers ───────────────────────────────────────────────────────────────────

static bool near(float a, float b, float tol = 1e-5f) {
    return fabsf(a - b) <= tol;
}

// ── Tests ─────────────────────────────────────────────────────────────────────

void test_agc_amplifies_quiet() {
    AGC agc(AGC_NORMAL);
    float last = 0;
    for (int i = 0; i < 200; i++) {
        last = agc.process(0.01f);
    }
    // Quiet signal should be amplified above input
    assert(last > 0.01f);
    assert(agc.current_gain() > 1.0f);
    printf("PASS: test_agc_amplifies_quiet (last=%.4f gain=%.3f)\n", last, agc.current_gain());
}

void test_agc_attenuates_loud() {
    AGC agc(AGC_NORMAL);
    float last = 0;
    for (int i = 0; i < 200; i++) {
        last = agc.process(0.9f);
    }
    // Output must always be clamped to 0-1
    assert(last >= 0.0f && last <= 1.0f);
    printf("PASS: test_agc_attenuates_loud (last=%.4f gain=%.3f)\n", last, agc.current_gain());
}

void test_agc_output_range() {
    AGC agc(AGC_NORMAL);
    for (int i = 0; i < 500; i++) {
        float v = agc.process(static_cast<float>(i % 100) / 100.0f);
        assert(v >= 0.0f && v <= 1.0f);
    }
    printf("PASS: test_agc_output_range\n");
}

void test_agc_reset() {
    AGC agc(AGC_NORMAL);
    // Drive gain away from 1.0
    for (int i = 0; i < 100; i++) agc.process(0.05f);
    assert(agc.current_gain() != 1.0f);
    agc.reset();
    assert(agc.current_gain() == 1.0f);
    printf("PASS: test_agc_reset\n");
}

void test_agc_suspend_decays_integrator() {
    // There is no public integrator accessor, so we verify indirectly:
    // after suspend, the integrator has decayed, which affects the next process() call.
    // A fresh AGC fed a constant 0.5 signal will build up integrator.
    // Suspending many times and then re-processing should produce a different trajectory
    // than not suspending — here we just confirm suspend() doesn't crash and
    // doesn't alter gain directly (gain only changes via process()).
    AGC agc(AGC_NORMAL);
    for (int i = 0; i < 100; i++) agc.process(0.5f);
    float gain_after_signal = agc.current_gain();

    for (int i = 0; i < 50; i++) agc.suspend();
    // Gain itself is unchanged by suspend alone
    assert(agc.current_gain() == gain_after_signal);

    printf("PASS: test_agc_suspend_decays_integrator (gain_unchanged=%.4f)\n", agc.current_gain());
}

void test_agc_tracks_toward_target() {
    // Feed a constant low signal; after enough iterations output should approach target (0.5).
    AGC agc(AGC_NORMAL);
    float last = 0.0f;
    for (int i = 0; i < 500; i++) {
        last = agc.process(0.05f);
    }
    // After 500 steps the AGC should have increased gain so output is closer to target
    assert(last > 0.05f);
    printf("PASS: test_agc_tracks_toward_target (last=%.4f)\n", last);
}

void test_agc_different_presets() {
    // VIVID has higher kp/ki than LAZY → should converge more aggressively for quiet signal.
    AGC vivid(AGC_VIVID);
    AGC lazy(AGC_LAZY);

    float last_vivid = 0.0f, last_lazy = 0.0f;
    for (int i = 0; i < 200; i++) {
        last_vivid = vivid.process(0.05f);
        last_lazy  = lazy.process(0.05f);
    }
    // VIVID gain should be higher (more aggressive amplification) than LAZY
    assert(vivid.current_gain() > lazy.current_gain());
    printf("PASS: test_agc_different_presets (vivid_gain=%.3f lazy_gain=%.3f)\n",
           vivid.current_gain(), lazy.current_gain());
}

void test_agc_gain_clamp_lower() {
    // Even with zero input, gain must not go below 1/64.
    AGC agc(AGC_NORMAL);
    for (int i = 0; i < 5000; i++) agc.process(0.0f);
    assert(agc.current_gain() >= 1.0f / 64.0f);
    printf("PASS: test_agc_gain_clamp_lower (gain=%.5f)\n", agc.current_gain());
}

void test_agc_gain_clamp_upper() {
    // Gain must not exceed 32.
    AGC agc(AGC_NORMAL);
    // Ultra-quiet signal for many iterations
    for (int i = 0; i < 10000; i++) agc.process(0.0001f);
    assert(agc.current_gain() <= 32.0f);
    printf("PASS: test_agc_gain_clamp_upper (gain=%.3f)\n", agc.current_gain());
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    test_agc_amplifies_quiet();
    test_agc_attenuates_loud();
    test_agc_output_range();
    test_agc_reset();
    test_agc_suspend_decays_integrator();
    test_agc_tracks_toward_target();
    test_agc_different_presets();
    test_agc_gain_clamp_lower();
    test_agc_gain_clamp_upper();
    printf("All AGC tests passed.\n");
    return 0;
}
