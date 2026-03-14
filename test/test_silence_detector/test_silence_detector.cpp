#include <cassert>
#include <cstdio>

#include "../../components/audio_reactive/silence_detector.h"

using namespace esphome::audio_reactive;

void test_below_gate_immediate() {
    // squelch=10 → threshold=5. Signal below threshold triggers gate.
    SilenceDetector det(10.0f);
    auto r = det.update(2.0f, 0);  // 2 < 5
    assert(r.is_below_gate);
    assert(!r.is_silent);
    printf("PASS: test_below_gate_immediate\n");
}

void test_silent_after_1000ms() {
    SilenceDetector det(10.0f);  // threshold = 5
    auto r0 = det.update(2.0f, 0);
    assert(r0.is_below_gate);
    assert(!r0.is_silent);

    auto r999 = det.update(2.0f, 999);
    assert(r999.is_below_gate);
    assert(!r999.is_silent);

    auto r1000 = det.update(2.0f, 1000);
    assert(r1000.is_below_gate);
    assert(r1000.is_silent);

    printf("PASS: test_silent_after_1000ms\n");
}

void test_above_gate_resets_flags() {
    SilenceDetector det(10.0f);  // threshold = 5
    det.update(2.0f, 0);
    det.update(2.0f, 1000);
    auto r_silent = det.update(2.0f, 1500);
    assert(r_silent.is_below_gate && r_silent.is_silent);

    // Signal rises above gate (music playing, mid+high > 5)
    auto r_above = det.update(15.0f, 2000);
    assert(!r_above.is_below_gate);
    assert(!r_above.is_silent);

    printf("PASS: test_above_gate_resets_flags\n");
}

void test_squelch_adjustment() {
    // squelch=5 → threshold=2.5, squelch=50 → threshold=25
    SilenceDetector det_low(5.0f);
    SilenceDetector det_high(50.0f);

    float signal = 10.0f;
    auto r_low  = det_low.update(signal, 0);
    auto r_high = det_high.update(signal, 0);

    // signal (10) > threshold_low (2.5) → not below gate
    assert(!r_low.is_below_gate);
    // signal (10) < threshold_high (25) → below gate
    assert(r_high.is_below_gate);

    printf("PASS: test_squelch_adjustment\n");
}

void test_zero_squelch_almost_no_gate() {
    // At squelch=0, threshold=0.0 — nothing triggers gate
    SilenceDetector det(0.0f);
    auto r = det.update(0.001f, 0);
    assert(!r.is_below_gate);
    auto r_zero = det.update(0.0f, 0);
    assert(!r_zero.is_below_gate);
    printf("PASS: test_zero_squelch_almost_no_gate\n");
}

void test_gate_restart_after_above() {
    SilenceDetector det(10.0f);  // threshold = 5
    det.update(2.0f, 0);
    det.update(2.0f, 700);
    // Signal rises momentarily
    det.update(15.0f, 800);
    // Back below gate — timer restarts
    det.update(2.0f, 850);
    // 849ms since restart — not silent
    auto r = det.update(2.0f, 1699);
    assert(r.is_below_gate);
    assert(!r.is_silent);
    // 1000ms since restart — now silent
    auto r2 = det.update(2.0f, 1850);
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
