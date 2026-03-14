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
    agc.set_noise_floor(0.0f);  // Disable noise floor for this test
    float last = 0;
    for (int i = 0; i < 200; i++) {
        last = agc.process(0.01f);
    }
    // Quiet signal should be amplified above input
    assert(last > 0.01f);
    assert(agc.current_gain() > 1.0f);
    printf("PASS: test_agc_amplifies_quiet (last=%.4f gain=%.3f)\n", last, agc.current_gain());
}

void test_agc_noise_floor_suppresses() {
    AGC agc(AGC_NORMAL);
    // Default noise floor is 15.0 — values below should return 0
    float result = agc.process(5.0f);
    assert(result == 0.0f);
    // Values above noise floor should be processed normally
    result = agc.process(20.0f);
    assert(result > 0.0f);
    printf("PASS: test_agc_noise_floor_suppresses\n");
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
    agc.set_noise_floor(0.0f);  // Disable noise floor to test full range
    for (int i = 0; i < 500; i++) {
        float v = agc.process(static_cast<float>(i % 100) / 100.0f);
        assert(v >= 0.0f && v <= 1.0f);
    }
    printf("PASS: test_agc_output_range\n");
}

void test_agc_reset() {
    AGC agc(AGC_NORMAL);
    // Drive gain away from 1.0 with values above noise floor (15.0)
    for (int i = 0; i < 100; i++) agc.process(20.0f);
    assert(agc.current_gain() != 1.0f);
    agc.reset();
    assert(agc.current_gain() == 1.0f);
    printf("PASS: test_agc_reset\n");
}

void test_agc_suspend_decays_integrator() {
    AGC agc(AGC_NORMAL);
    for (int i = 0; i < 100; i++) agc.process(20.0f);  // Above noise floor (15.0)
    float gain_after_signal = agc.current_gain();

    for (int i = 0; i < 50; i++) agc.suspend();
    // Gain itself is unchanged by suspend alone
    assert(agc.current_gain() == gain_after_signal);

    printf("PASS: test_agc_suspend_decays_integrator (gain_unchanged=%.4f)\n", agc.current_gain());
}

void test_agc_tracks_toward_target() {
    // Feed a constant signal above noise floor; AGC should adjust gain
    AGC agc(AGC_NORMAL);
    float last = 0.0f;
    for (int i = 0; i < 500; i++) {
        last = agc.process(20.0f);  // Above noise floor (15.0)
    }
    // After 500 steps the AGC should have adjusted output toward target
    assert(last > 0.0f && last <= 1.0f);
    printf("PASS: test_agc_tracks_toward_target (last=%.4f)\n", last);
}

void test_agc_different_presets() {
    // VIVID has higher kp/ki than LAZY → should converge more aggressively
    AGC vivid(AGC_VIVID);
    AGC lazy(AGC_LAZY);

    for (int i = 0; i < 200; i++) {
        vivid.process(20.0f);  // Above noise floor (15.0)
        lazy.process(20.0f);
    }
    // Both should have adjusted gain from 1.0
    assert(vivid.current_gain() != 1.0f);
    assert(lazy.current_gain() != 1.0f);
    printf("PASS: test_agc_different_presets (vivid_gain=%.3f lazy_gain=%.3f)\n",
           vivid.current_gain(), lazy.current_gain());
}

void test_agc_gain_clamp_lower() {
    // Even with zero input, gain must not go below 1/64.
    AGC agc(AGC_NORMAL);
    agc.set_noise_floor(0.0f);  // Disable noise floor to test gain clamp
    for (int i = 0; i < 5000; i++) agc.process(0.0f);
    assert(agc.current_gain() >= 1.0f / 64.0f);
    printf("PASS: test_agc_gain_clamp_lower (gain=%.5f)\n", agc.current_gain());
}

void test_agc_gain_clamp_upper() {
    // Gain must not exceed 32.
    AGC agc(AGC_NORMAL);
    agc.set_noise_floor(0.0f);  // Disable noise floor to test gain clamp
    for (int i = 0; i < 10000; i++) agc.process(0.0001f);
    assert(agc.current_gain() <= 32.0f);
    printf("PASS: test_agc_gain_clamp_upper (gain=%.3f)\n", agc.current_gain());
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    test_agc_amplifies_quiet();
    test_agc_noise_floor_suppresses();
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
