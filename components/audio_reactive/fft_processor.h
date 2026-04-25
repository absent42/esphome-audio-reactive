#pragma once

#include <cmath>
#include <cstring>

#ifdef AUDIO_REACTIVE_NATIVE_TEST
#include "../../test/third_party/kissfft.h"
#else
// arduinoFFT's defs.h #defines DDR(x) as ((x)-1) for AVR-ish "data direction
// register" math. ESP-IDF's xtensa specreg.h #defines DDR as 104 (debug-data
// register address). When both headers land in the same translation unit
// (esphome.h pulls FreeRTOS->xtensa, then audio_reactive.h pulls arduinoFFT)
// the DDR redefinition emits two warnings per compile. Save / undef the
// xtensa macro before including arduinoFFT, then restore after — arduinoFFT's
// DDR ends up shadowed by the restored xtensa macro, and nobody downstream
// sees the redefinition.
#pragma push_macro("DDR")
#undef DDR
#include <arduinoFFT.h>
#pragma pop_macro("DDR")
#endif

namespace esphome {
namespace audio_reactive {

/// Wraps ArduinoFFT to produce frequency bin magnitudes from raw samples.
/// Template parameter N is the FFT size (must be power of 2).
template <size_t N>
class FFTProcessor {
 public:
    explicit FFTProcessor(float sample_rate)
        : sample_rate_(sample_rate) {
        // Pre-compute Hamming window coefficients once at construction.
        // Avoids recomputing 512 cosines per hop (~88K trig calls/sec at 172 Hz).
        for (size_t i = 0; i < N; i++) {
            window_[i] = 0.54f - 0.46f * cosf(2.0f * static_cast<float>(M_PI) * i / (N - 1));
        }
#ifdef AUDIO_REACTIVE_NATIVE_TEST
        kiss_cfg_ = kiss_fftr_alloc(static_cast<int>(N), 0, nullptr, nullptr);
#endif
    }

#ifdef AUDIO_REACTIVE_NATIVE_TEST
    ~FFTProcessor() {
        if (kiss_cfg_ != nullptr) {
            kiss_fftr_free(kiss_cfg_);
            kiss_cfg_ = nullptr;
        }
    }
    // Non-copyable / non-movable because we own a kissfft allocation.
    FFTProcessor(const FFTProcessor &) = delete;
    FFTProcessor &operator=(const FFTProcessor &) = delete;
#endif

    /// Number of usable frequency bins (N/2).
    constexpr size_t bin_count() const { return N / 2; }

    /// Frequency resolution in Hz per bin.
    float frequency_resolution() const { return sample_rate_ / static_cast<float>(N); }

    /// Which bin index corresponds to a given frequency.
    size_t bin_for_frequency(float freq) const {
        size_t bin = static_cast<size_t>(freq / frequency_resolution());
        return (bin < bin_count()) ? bin : bin_count() - 1;
    }

    /// Run FFT on input samples and compute magnitudes and phases.
    /// Input array must have exactly N elements.
    void process(const float* samples) {
#ifdef AUDIO_REACTIVE_NATIVE_TEST
        // Apply the Hamming window into the real buffer that kissfft consumes.
        for (size_t i = 0; i < N; i++) {
            real_[i] = samples[i] * window_[i];
        }
        // kiss_fftr writes N/2 + 1 complex bins; we only use the first N/2 to
        // match the arduinoFFT-based interface (no Nyquist bin exposed).
        kiss_fftr(kiss_cfg_, real_, kiss_out_);
        // Scale by 1/N so magnitudes match the naive DFT this branch used to
        // compute (test_fft_processor asserts only on peak bin location, but
        // downstream mel/band code expects consistent magnitude units).
        const float inv_n = 1.0f / static_cast<float>(N);
        for (size_t i = 0; i < N / 2; i++) {
            const float re = kiss_out_[i].r;
            const float im = kiss_out_[i].i;
            magnitudes_[i] = sqrtf(re * re + im * im) * inv_n;
            phases_[i] = atan2f(im, re);
        }
#else
        // Apply pre-computed Hamming window during copy
        for (size_t i = 0; i < N; i++) {
            real_[i] = samples[i] * window_[i];
            imag_[i] = 0.0f;
        }
        ArduinoFFT<float> fft(real_, imag_, N, sample_rate_);
        // Window already applied above — skip fft.windowing()
        fft.compute(FFTDirection::Forward);

        // Extract phases BEFORE complexToMagnitude destroys complex data
        for (size_t i = 0; i < N / 2; i++) {
            phases_[i] = atan2f(imag_[i], real_[i]);
        }

        fft.complexToMagnitude();
        for (size_t i = 0; i < N / 2; i++) {
            magnitudes_[i] = real_[i];
        }
#endif
    }

    /// Pointer to magnitude array (bin_count() elements).
    const float* magnitudes() const { return magnitudes_; }

    /// Pointer to phase array (bin_count() elements, in radians).
    const float* phases() const { return phases_; }

 private:
    float sample_rate_;
    float window_[N]{};         // Pre-computed Hamming window coefficients
    float real_[N]{};
#ifdef AUDIO_REACTIVE_NATIVE_TEST
    // kiss_fftr produces N/2 + 1 complex bins (0..Nyquist). We reserve room
    // for all of them even though the exposed magnitudes_/phases_ arrays only
    // hold N/2 (the arduinoFFT interface doesn't expose Nyquist either).
    kiss_fft_cpx kiss_out_[N / 2 + 1]{};
    kiss_fftr_cfg kiss_cfg_{nullptr};
#else
    float imag_[N]{};
#endif
    float magnitudes_[N / 2]{};
    float phases_[N / 2]{};
};

}  // namespace audio_reactive
}  // namespace esphome
