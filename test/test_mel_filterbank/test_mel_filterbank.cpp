#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#define AUDIO_REACTIVE_NATIVE_TEST
#include "../../components/audio_reactive/mel_filterbank.h"

using namespace esphome::audio_reactive;

// A small dedicated type for tests at reasonable size.
using MelFB = MelFilterbank<32, 2048>;

static std::vector<float> make_sine_magnitude_squared(float sample_rate, float hz, uint16_t n_fft) {
    // Fake a squared-magnitude spectrum peaked at the bin nearest to `hz`.
    // Real spectrum would have spectral leakage; a sharp peak is enough for
    // a unit test that mel bands correctly localize energy.
    std::vector<float> mag(n_fft / 2, 0.0f);
    float bin_hz = sample_rate / n_fft;
    uint16_t bin = static_cast<uint16_t>(hz / bin_hz);
    if (bin < mag.size()) mag[bin] = 1.0f;
    return mag;
}

void test_band_count() {
    MelFB fb;
    fb.setup(44100.0f);
    assert(MelFB::kNumBands == 32);
    printf("PASS: test_band_count\n");
}

void test_sine_hits_expected_band() {
    MelFB fb;
    fb.setup(44100.0f, 40.0f, 16000.0f);

    // A 1000 Hz tone should land roughly mid-filterbank (~band 8-20).
    auto mag = make_sine_magnitude_squared(44100.0f, 1000.0f, 2048);
    float bands[32] = {};
    fb.process(mag.data(), bands);

    // Find peak band (raw energy: largest value is the peak)
    uint16_t peak = 0;
    float peak_val = 0.0f;
    for (uint16_t i = 0; i < 32; i++) {
        if (bands[i] > peak_val) {
            peak_val = bands[i];
            peak = i;
        }
    }
    assert(peak >= 8 && peak <= 20);
    assert(peak_val > 0.0f);
    printf("PASS: test_sine_hits_expected_band (peak at band %u, val=%.4f)\n", peak, peak_val);
}

void test_zero_input_is_zero() {
    MelFB fb;
    fb.setup(44100.0f);
    std::vector<float> mag(1024, 0.0f);
    float bands[32] = {};
    fb.process(mag.data(), bands);
    for (uint16_t i = 0; i < 32; i++) {
        assert(bands[i] == 0.0f);
    }
    printf("PASS: test_zero_input_is_zero\n");
}

void test_white_noise_approximately_uniform() {
    MelFB fb;
    fb.setup(44100.0f);
    std::vector<float> mag(1024, 1.0f);  // Constant spectrum = white-ish
    float bands[32] = {};
    fb.process(mag.data(), bands);
    // With unit-energy normalization, each band sums weights to 1, so output ~= 1.0 on flat input.
    float min_v = bands[0], max_v = bands[0];
    for (uint16_t i = 1; i < 32; i++) {
        if (bands[i] < min_v) min_v = bands[i];
        if (bands[i] > max_v) max_v = bands[i];
    }
    assert(min_v > 0.9f && max_v < 1.1f);
    printf("PASS: test_white_noise_approximately_uniform (min=%.3f max=%.3f)\n", min_v, max_v);
}

int main() {
    test_band_count();
    test_sine_hits_expected_band();
    test_zero_input_is_zero();
    test_white_noise_approximately_uniform();
    printf("ALL MEL FILTERBANK TESTS PASSED\n");
    return 0;
}
