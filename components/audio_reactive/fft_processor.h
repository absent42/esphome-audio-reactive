#pragma once

#include <cmath>
#include <cstring>

#ifndef AUDIO_REACTIVE_NATIVE_TEST
#include <arduinoFFT.h>
#endif

namespace esphome {
namespace audio_reactive {

/// Wraps ArduinoFFT to produce frequency bin magnitudes from raw samples.
/// Template parameter N is the FFT size (must be power of 2).
template <size_t N>
class FFTProcessor {
 public:
    explicit FFTProcessor(float sample_rate)
        : sample_rate_(sample_rate) {}

    /// Number of usable frequency bins (N/2).
    constexpr size_t bin_count() const { return N / 2; }

    /// Frequency resolution in Hz per bin.
    float frequency_resolution() const { return sample_rate_ / static_cast<float>(N); }

    /// Which bin index corresponds to a given frequency.
    size_t bin_for_frequency(float freq) const {
        size_t bin = static_cast<size_t>(freq / frequency_resolution());
        return (bin < bin_count()) ? bin : bin_count() - 1;
    }

    /// Run FFT on input samples and compute magnitudes.
    /// Input array must have exactly N elements.
    void process(const float* samples) {
        // Copy to working buffers
        for (size_t i = 0; i < N; i++) {
            real_[i] = samples[i];
            imag_[i] = 0.0f;
        }

#ifdef AUDIO_REACTIVE_NATIVE_TEST
        // Minimal DFT for testing (slow but correct, no library dependency)
        for (size_t k = 0; k < N / 2; k++) {
            float sum_re = 0.0f, sum_im = 0.0f;
            for (size_t n = 0; n < N; n++) {
                float angle = 2.0f * static_cast<float>(M_PI) * k * n / N;
                sum_re += real_[n] * cosf(angle);
                sum_im -= real_[n] * sinf(angle);
            }
            magnitudes_[k] = sqrtf(sum_re * sum_re + sum_im * sum_im) / N;
        }
#else
        ArduinoFFT<float> fft(real_, imag_, N, sample_rate_);
        fft.windowing(FFTWindow::Hamming, FFTDirection::Forward);
        fft.compute(FFTDirection::Forward);
        fft.complexToMagnitude();
        for (size_t i = 0; i < N / 2; i++) {
            magnitudes_[i] = real_[i];
        }
#endif
    }

    /// Pointer to magnitude array (bin_count() elements).
    const float* magnitudes() const { return magnitudes_; }

 private:
    float sample_rate_;
    float real_[N]{};
    float imag_[N]{};
    float magnitudes_[N / 2]{};
};

}  // namespace audio_reactive
}  // namespace esphome
