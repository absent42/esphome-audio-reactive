#include <cassert>
#include <cstdio>

#include "../../components/audio_reactive/silence_detector.h"

using namespace esphome::audio_reactive;

void test_below_gate_immediate() {
    // A signal below the threshold should immediately set is_below_gate
    SilenceDetector det(10.0f);  // threshold = 0.01
    auto r = det.update(0.005f, 0);
    assert(r.is_below_gate);
    assert(!r.is_silent);  // Only below gate for 0ms, not yet silent
    printf("PASS: test_below_gate_immediate\n");
}

void test_silent_after_1000ms() {
    SilenceDetector det(10.0f);  // threshold = 0.01
    // Feed a signal below the gate for 0ms — not yet silent
    auto r0 = det.update(0.005f, 0);
    assert(r0.is_below_gate);
    assert(!r0.is_silent);

    // 999ms — still not silent
    auto r999 = det.update(0.005f, 999);
    assert(r999.is_below_gate);
    assert(!r999.is_silent);

    // 1000ms — now silent
    auto r1000 = det.update(0.005f, 1000);
    assert(r1000.is_below_gate);
    assert(r1000.is_silent);

    printf("PASS: test_silent_after_1000ms\n");
}

void test_above_gate_resets_flags() {
    SilenceDetector det(10.0f);
    // Go silent
    det.update(0.005f, 0);
    det.update(0.005f, 1000);
    auto r_silent = det.update(0.005f, 1500);
    assert(r_silent.is_below_gate && r_silent.is_silent);

    // Signal rises above gate
    auto r_above = det.update(0.5f, 2000);
    assert(!r_above.is_below_gate);
    assert(!r_above.is_silent);

    printf("PASS: test_above_gate_resets_flags\n");
}

void test_squelch_adjustment() {
    // Higher squelch means higher threshold — same signal that passed low squelch
    // should fail at high squelch.
    SilenceDetector det_low(5.0f);   // threshold = 0.005
    SilenceDetector det_high(50.0f); // threshold = 0.05

    float signal = 0.02f;
    auto r_low  = det_low.update(signal, 0);
    auto r_high = det_high.update(signal, 0);

    // signal (0.02) > threshold_low (0.005) → not below gate for low squelch
    assert(!r_low.is_below_gate);
    // signal (0.02) < threshold_high (0.05) → below gate for high squelch
    assert(r_high.is_below_gate);

    printf("PASS: test_squelch_adjustment\n");
}

void test_zero_squelch_almost_no_gate() {
    // At squelch=0, threshold=0.0 — no signal is strictly less than 0,
    // so nothing triggers the gate (not even 0.0f, since 0.0f < 0.0f is false).
    SilenceDetector det(0.0f);
    // A tiny positive signal should pass through (not be below gate)
    auto r = det.update(0.001f, 0);
    assert(!r.is_below_gate);
    // Zero signal: 0.0 < 0.0 is false, so also not below gate
    auto r_zero = det.update(0.0f, 0);
    assert(!r_zero.is_below_gate);
    printf("PASS: test_zero_squelch_almost_no_gate\n");
}

void test_gate_restart_after_above() {
    // If signal goes above gate briefly then drops back below,
    // the 1s timer should restart.
    SilenceDetector det(10.0f);
    det.update(0.005f, 0);
    det.update(0.005f, 700);
    // Signal rises momentarily
    det.update(0.5f, 800);
    // Back below gate — timer restarts
    det.update(0.005f, 850);
    // 849ms since restart — not silent
    auto r = det.update(0.005f, 1699);
    assert(r.is_below_gate);
    assert(!r.is_silent);
    // 1000ms since restart — now silent
    auto r2 = det.update(0.005f, 1850);
    assert(r2.is_silent);
    printf("PASS: test_gate_restart_after_above\n");
}

void test_set_squelch_clamping() {
    SilenceDetector det(50.0f);
    det.set_squelch(-10.0f);
    assert(det.squelch() == 0.0f);
    det.set_squelch(200.0f);
    assert(det.squelch() == 100.0f);
    det.set_squelch(42.0f);
    assert(det.squelch() == 42.0f);
    printf("PASS: test_set_squelch_clamping\n");
}

int main() {
    test_below_gate_immediate();
    test_silent_after_1000ms();
    test_above_gate_resets_flags();
    test_squelch_adjustment();
    test_zero_squelch_almost_no_gate();
    test_gate_restart_after_above();
    test_set_squelch_clamping();
    printf("All silence detector tests passed.\n");
    return 0;
}
