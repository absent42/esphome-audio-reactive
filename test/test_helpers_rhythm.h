#pragma once
// Synthetic onset-envelope generators for tempo tests. Club patterns and
// speech-like noise are the fixtures that expose the 114/150 attractor bugs
// that pure metronome trains cannot (see docs/plans/2026-07-02-tempo-
// induction-rewrite.md, Background).
#include <cmath>
#include <cstdint>

namespace rhythm_fixtures {

// Deterministic LCG so tests are reproducible.
inline uint32_t &rng_state() { static uint32_t s = 12345; return s; }
inline void seed(uint32_t s) { rng_state() = s; }
inline float frand() {
    rng_state() = rng_state() * 1664525u + 1013904223u;
    return (rng_state() >> 8) * (1.0f / 16777216.0f);
}

inline void add_onset(float *env, int n, float t, float amp) {
    int f = (int)(t + 0.5f) + (int)((frand() * 2.0f - 1.0f) + 0.5f);
    amp *= (0.8f + 0.4f * frand());  // amplitude jitter
    if (f >= 0 && f < n) env[f] += amp;          // 3-frame smear: SuperFlux
    if (f + 1 < n && f + 1 >= 0) env[f + 1] += amp * 0.4f;  // flux is not an
    if (f - 1 >= 0 && f - 1 < n) env[f - 1] += amp * 0.2f;  // impulse
}

// Club pattern at `fps` frames/s: accented kick on 1 (16), kicks (10),
// snares on 2 and 4 (12), eighth-note hats (7), sixteenth ghosts (3),
// noisy baseline. Returns frames written.
inline int build_club(float bpm, float seconds, float fps, float *env, int max_len) {
    int n = (int)(fps * seconds);
    if (n > max_len) n = max_len;
    for (int i = 0; i < n; i++) env[i] = 0.05f + 0.15f * frand();
    const float period = fps * 60.0f / bpm;
    float t = 5.0f;
    int beat_in_bar = 0;
    while (t < n) {
        float amp = (beat_in_bar == 0) ? 16.0f : 10.0f;
        if (beat_in_bar == 1 || beat_in_bar == 3) amp = 12.0f;
        add_onset(env, n, t, amp);
        add_onset(env, n, t + period * 0.50f, 7.0f);
        add_onset(env, n, t + period * 0.25f, 3.0f);
        add_onset(env, n, t + period * 0.75f, 3.0f);
        t += period;
        beat_in_bar = (beat_in_bar + 1) & 3;
    }
    return n;
}

// Speech/TV-like envelope: random onsets, no periodicity.
inline int build_speech(float seconds, float fps, float *env, int max_len) {
    int n = (int)(fps * seconds);
    if (n > max_len) n = max_len;
    for (int i = 0; i < n; i++) env[i] = 0.05f + 0.15f * frand();
    for (int v = 0; v < (int)(seconds * 3.5f); v++)
        add_onset(env, n, frand() * n, 2.0f + 6.0f * frand());
    return n;
}

}  // namespace rhythm_fixtures
