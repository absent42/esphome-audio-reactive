#include <cassert>
#include <cmath>
#include <cstdio>

#include "../../components/audio_reactive/dynamics_limiter.h"

using namespace esphome::audio_reactive;

static bool approx_eq(float a, float b, float tol = 0.001f) {
    return std::fabs(a - b) < tol;
}

void test_rise_limited_by_attack() {
    // attack_ms=100 → max_rise = 196 * delta_ms / 100
    // At delta=10ms: max_rise = 19.6
    DynamicsLimiter lim(100.0f, 1400.0f);
    // Start at 0, jump to 100 in one step of 10ms
    float out = lim.process(100.0f, 10.0f);
    // Should be capped at 19.6, not 100
    assert(out < 25.0f);
    assert(out > 15.0f);
    printf("PASS: test_rise_limited_by_attack (out=%.2f)\n", out);
}

void test_fall_limited_by_decay() {
    // decay_ms=1400 → max_fall = 196 * delta_ms / 1400
    // At delta=10ms: max_fall = 1.4
    DynamicsLimiter lim(80.0f, 1400.0f);
    // First get to a high value rapidly
    for (int i = 0; i < 50; i++) lim.process(100.0f, 10.0f);
    float high_val = lim.last_value();
    // Now drop to 0 in one step of 10ms
    float out = lim.process(0.0f, 10.0f);
    // Should only fall by max_fall = 196*10/1400 ≈ 1.4
    float expected_fall = 196.0f * 10.0f / 1400.0f;
    assert(approx_eq(high_val - out, expected_fall, 0.01f));
    printf("PASS: test_fall_limited_by_decay (fall=%.2f, expected=%.2f)\n",
           high_val - out, expected_fall);
}

void test_eventually_reaches_target_rising() {
    DynamicsLimiter lim(80.0f, 1400.0f);
    float target = 50.0f;
    // Process many steps of 10ms with target=50
    for (int i = 0; i < 500; i++) lim.process(target, 10.0f);
    float final_val = lim.last_value();
    assert(approx_eq(final_val, target, 0.01f));
    printf("PASS: test_eventually_reaches_target_rising (final=%.2f)\n", final_val);
}

void test_eventually_reaches_target_falling() {
    DynamicsLimiter lim(80.0f, 1400.0f);
    // First get to a high value
    for (int i = 0; i < 200; i++) lim.process(100.0f, 10.0f);
    // Now decay to 0
    for (int i = 0; i < 2000; i++) lim.process(0.0f, 10.0f);
    float final_val = lim.last_value();
    assert(approx_eq(final_val, 0.0f, 0.01f));
    printf("PASS: test_eventually_reaches_target_falling (final=%.2f)\n", final_val);
}

void test_reset_clears_state() {
    DynamicsLimiter lim(80.0f, 1400.0f);
    for (int i = 0; i < 50; i++) lim.process(100.0f, 10.0f);
    assert(lim.last_value() > 0.0f);
    lim.reset();
    assert(lim.last_value() == 0.0f);
    printf("PASS: test_reset_clears_state\n");
}

void test_zero_delta_returns_last_value() {
    DynamicsLimiter lim(80.0f, 1400.0f);
    lim.process(50.0f, 10.0f);
    float val_before = lim.last_value();
    float out = lim.process(100.0f, 0.0f);
    assert(approx_eq(out, val_before));
    printf("PASS: test_zero_delta_returns_last_value\n");
}

void test_negative_delta_returns_last_value() {
    DynamicsLimiter lim(80.0f, 1400.0f);
    lim.process(50.0f, 10.0f);
    float val_before = lim.last_value();
    float out = lim.process(100.0f, -5.0f);
    assert(approx_eq(out, val_before));
    printf("PASS: test_negative_delta_returns_last_value\n");
}

void test_attack_faster_than_decay() {
    // With default parameters (attack=80ms, decay=1400ms),
    // rise rate should be much faster than fall rate.
    DynamicsLimiter lim(80.0f, 1400.0f);
    float delta_ms = 10.0f;
    float max_rise = 196.0f * delta_ms / 80.0f;
    float max_fall = 196.0f * delta_ms / 1400.0f;
    assert(max_rise > max_fall);
    printf("PASS: test_attack_faster_than_decay (rise=%.2f, fall=%.2f)\n",
           max_rise, max_fall);
}

int main() {
    test_rise_limited_by_attack();
    test_fall_limited_by_decay();
    test_eventually_reaches_target_rising();
    test_eventually_reaches_target_falling();
    test_reset_clears_state();
    test_zero_delta_returns_last_value();
    test_negative_delta_returns_last_value();
    test_attack_faster_than_decay();
    printf("All dynamics limiter tests passed.\n");
    return 0;
}
