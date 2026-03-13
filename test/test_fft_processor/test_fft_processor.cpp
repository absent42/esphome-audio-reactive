#include <cassert>
#include <cmath>
#include <cstdio>

// Stub arduinoFFT for native testing
#define AUDIO_REACTIVE_NATIVE_TEST
#include "../../components/audio_reactive/fft_processor.h"

using namespace esphome::audio_reactive;

void test_bin_count() {
    FFTProcessor<512> proc(16000.0f);
    assert(proc.bin_count() == 256);
    printf("PASS: test_bin_count\n");
}

void test_frequency_resolution() {
    FFTProcessor<512> proc(16000.0f);
    float res = proc.frequency_resolution();
    assert(res > 31.0f && res < 32.0f);
    printf("PASS: test_frequency_resolution\n");
}

void test_bin_for_frequency() {
    FFTProcessor<512> proc(16000.0f);
    size_t bin = proc.bin_for_frequency(100.0f);
    assert(bin >= 2 && bin <= 4);
    printf("PASS: test_bin_for_frequency\n");
}

void test_process_sine_wave() {
    constexpr size_t N = 512;
    FFTProcessor<N> proc(16000.0f);
    float samples[N];
    for (size_t i = 0; i < N; i++) {
        samples[i] = sinf(2.0f * M_PI * 200.0f * i / 16000.0f);
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
    assert(peak_bin >= 5 && peak_bin <= 8);
    assert(peak_val > 0.0f);
    printf("PASS: test_process_sine_wave (peak at bin %zu)\n", peak_bin);
}

int main() {
    test_bin_count();
    test_frequency_resolution();
    test_bin_for_frequency();
    test_process_sine_wave();
    printf("All FFT processor tests passed.\n");
    return 0;
}
