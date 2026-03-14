#include <cassert>
#include <cmath>
#include <cstdio>

// Stub arduinoFFT for native testing
#define AUDIO_REACTIVE_NATIVE_TEST
#include "../../components/audio_reactive/fft_processor.h"

using namespace esphome::audio_reactive;

void test_bin_count() {
    FFTProcessor<512> proc(22050.0f);
    assert(proc.bin_count() == 256);
    printf("PASS: test_bin_count\n");
}

void test_frequency_resolution() {
    FFTProcessor<512> proc(22050.0f);
    float res = proc.frequency_resolution();
    // 22050 / 512 ≈ 43.07
    assert(res > 43.0f && res < 43.2f);
    printf("PASS: test_frequency_resolution\n");
}

void test_bin_for_frequency() {
    FFTProcessor<512> proc(22050.0f);
    size_t bin = proc.bin_for_frequency(200.0f);
    // 200 / 43.07 ≈ 4.6, expect bin 4 or 5
    assert(bin >= 3 && bin <= 6);
    printf("PASS: test_bin_for_frequency\n");
}

void test_process_sine_wave() {
    constexpr size_t N = 512;
    FFTProcessor<N> proc(22050.0f);
    float samples[N];
    // 200 Hz sine at 22050 Hz sample rate
    for (size_t i = 0; i < N; i++) {
        samples[i] = sinf(2.0f * M_PI * 200.0f * i / 22050.0f);
    }
    proc.process(samples);
    const float* magnitudes = proc.magnitudes();

    size_t peak_bin = 0;
    float peak_val = 0.0f;
    for (size_t i = 1; i < N / 2; i++) {
        if (magnitudes[i] > peak_val) {
            peak_val = magnitudes[i];
            peak_bin = i;
        }
    }
    // 200 Hz / 43.07 Hz-per-bin ≈ bin 4-5
    assert(peak_bin >= 3 && peak_bin <= 7);
    assert(peak_val > 0.0f);
    printf("PASS: test_process_sine_wave (peak at bin %zu)\n", peak_bin);
}

void test_frequency_resolution_22050() {
    FFTProcessor<512> fft(22050.0f);
    float res = fft.frequency_resolution();
    assert(res > 43.0f && res < 43.2f);
    printf("PASS: test_frequency_resolution_22050\n");
}

void test_pink_noise_coefficients() {
    FFTProcessor<512> fft(22050.0f);
    const float *coeffs = fft.pink_noise_coefficients();
    assert(coeffs[0] > 1.5f && coeffs[0] < 1.9f);
    assert(coeffs[15] > 9.0f && coeffs[15] < 10.0f);
    printf("PASS: test_pink_noise_coefficients\n");
}

int main() {
    test_bin_count();
    test_frequency_resolution();
    test_bin_for_frequency();
    test_process_sine_wave();
    test_frequency_resolution_22050();
    test_pink_noise_coefficients();
    printf("All FFT processor tests passed.\n");
    return 0;
}
