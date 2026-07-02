// End-to-end pipeline test: WAV fixture + JSON ground truth -> pro-tier
// DSP (FFT -> mel -> musical bands -> SuperFlux onset -> BTrack) ->
// BPM convergence + onset F-measure assertions.
//
// Fixtures live in test/fixtures/audio/. See that directory's README.md
// for the format and license rules.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define AUDIO_REACTIVE_NATIVE_TEST
// MUSICAL_BANDS_USE_REAL_AGC: opt the e2e pipeline test in to the real AGC
// class instead of the passthrough stub the smaller native unit tests use.
// AGC dynamics (gain wind-up, sample_avg time constants, asymmetric EMA
// after AGC clipping) are exactly what this test is designed to exercise,
// so a stub would mask the bugs we are trying to detect.
#define MUSICAL_BANDS_USE_REAL_AGC
#include "../../components/audio_reactive/btrack.h"
#include "../../components/audio_reactive/fft_processor.h"
#include "../../components/audio_reactive/mel_filterbank.h"
#include "../../components/audio_reactive/musical_bands.h"
#include "../../components/audio_reactive/superflux_onset.h"

// dr_wav single-header WAV loader (public domain).
#define DR_WAV_IMPLEMENTATION
#include "../third_party/dr_wav.h"

using namespace esphome::audio_reactive;

// ---- Fixture discovery ------------------------------------------------------
//
// PlatformIO's native test runner executes binaries with an indeterminate CWD
// depending on version; to make the test robust we derive the fixture
// directory from __FILE__ at compile time. __FILE__ is typically an absolute
// path to this source; fixtures live in <repo_root>/test/fixtures/audio.

static std::string source_dir() {
    // __FILE__ is ".../test/test_e2e_pro_pipeline/test_e2e_pro_pipeline.cpp".
    // Strip the trailing filename to get the directory.
    std::string f = __FILE__;
    auto slash = f.find_last_of("/\\");
    if (slash == std::string::npos) return ".";
    return f.substr(0, slash);
}

static std::string fixture_dir() {
    // test/test_e2e_pro_pipeline/ -> test/fixtures/audio/
    return source_dir() + "/../fixtures/audio";
}

// ---- Lightweight directory listing (POSIX) ----------------------------------

#include <dirent.h>

static std::vector<std::string> list_wavs(const std::string &dir) {
    std::vector<std::string> out;
    DIR *d = opendir(dir.c_str());
    if (!d) return out;
    for (dirent *e = readdir(d); e != nullptr; e = readdir(d)) {
        std::string name = e->d_name;
        if (name.size() < 4) continue;
        if (name.compare(name.size() - 4, 4, ".wav") != 0) continue;
        out.push_back(name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// ---- Fixture loading --------------------------------------------------------

struct Fixture {
    std::string name;          // filename stem (e.g. "metronome_120bpm")
    std::vector<float> pcm;    // mono float samples in [-1, 1]
    float sample_rate;
    float bpm;
    std::vector<float> onset_times;  // seconds
};

static bool read_file(const std::string &path, std::string *out) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) {
        std::fclose(f);
        return false;
    }
    out->resize(static_cast<size_t>(n));
    size_t got = std::fread(&(*out)[0], 1, static_cast<size_t>(n), f);
    std::fclose(f);
    return got == static_cast<size_t>(n);
}

// Tiny JSON slicer for the fixed shape produced by _generate_metronome.py.
// Only extracts two fields: "bpm" (int) and "onset_times" (array of floats).
// We purposely avoid vendoring a JSON library for this single use.
static bool parse_annotation(const std::string &json, float *out_bpm,
                             std::vector<float> *out_onsets) {
    // BPM
    auto bpm_pos = json.find("\"bpm\"");
    if (bpm_pos == std::string::npos) return false;
    auto colon = json.find(':', bpm_pos);
    if (colon == std::string::npos) return false;
    *out_bpm = static_cast<float>(std::strtod(json.c_str() + colon + 1, nullptr));

    // onset_times
    auto arr_key = json.find("\"onset_times\"");
    if (arr_key == std::string::npos) return false;
    auto open_bracket = json.find('[', arr_key);
    if (open_bracket == std::string::npos) return false;
    auto close_bracket = json.find(']', open_bracket);
    if (close_bracket == std::string::npos) return false;

    out_onsets->clear();
    const char *p = json.c_str() + open_bracket + 1;
    const char *end = json.c_str() + close_bracket;
    while (p < end) {
        while (p < end && (*p == ',' || *p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) {
            ++p;
        }
        if (p >= end) break;
        char *next = nullptr;
        double v = std::strtod(p, &next);
        if (next == p) break;
        out_onsets->push_back(static_cast<float>(v));
        p = next;
    }
    return true;
}

static bool load_fixture(const std::string &stem, Fixture *out) {
    const std::string dir = fixture_dir();
    const std::string wav_path = dir + "/" + stem + ".wav";
    const std::string json_path = dir + "/" + stem + ".json";

    // WAV via dr_wav.
    unsigned int channels = 0;
    unsigned int sample_rate = 0;
    drwav_uint64 total_frames = 0;
    float *pcm = drwav_open_file_and_read_pcm_frames_f32(
        wav_path.c_str(), &channels, &sample_rate, &total_frames, nullptr);
    if (!pcm) {
        std::fprintf(stderr, "ERROR: failed to open %s\n", wav_path.c_str());
        return false;
    }
    if (channels != 1) {
        std::fprintf(stderr, "ERROR: fixture %s is not mono (channels=%u)\n",
                     wav_path.c_str(), channels);
        drwav_free(pcm, nullptr);
        return false;
    }
    out->name = stem;
    out->sample_rate = static_cast<float>(sample_rate);
    out->pcm.assign(pcm, pcm + total_frames);
    drwav_free(pcm, nullptr);

    // JSON annotations.
    std::string raw;
    if (!read_file(json_path, &raw)) {
        std::fprintf(stderr, "ERROR: failed to read %s\n", json_path.c_str());
        return false;
    }
    if (!parse_annotation(raw, &out->bpm, &out->onset_times)) {
        std::fprintf(stderr, "ERROR: could not parse %s\n", json_path.c_str());
        return false;
    }
    return true;
}

// ---- Pipeline driver --------------------------------------------------------

struct FrameOut {
    float bpm;
    float beat_phase;
    float beat_confidence;
    bool beat_event;
    float superflux_strength;
    bool superflux_event;
    // Per-musical-band smoothed AGC output [0, 1]. Captured so the metrics
    // step can detect AGC saturation (mean stuck near 1.0) and band-level
    // dynamic-range failure modes that the BPM / F-measure thresholds do
    // not catch.
    float bands[MusicalBands::kNumBands];
    // Raw per-band mel sum (BEFORE AGC processing). Stored so the test can
    // distinguish "AGC under-amplifying" from "no signal in mel slot" — a
    // band that's at zero in `bands` but has non-zero raw sum points to
    // AGC dynamics; a band that's zero in both means the mel filter isn't
    // producing energy there for this fixture.
    float raw_band_sum[MusicalBands::kNumBands];
};

static constexpr size_t kFFTSize = 2048;
static constexpr size_t kHopSize = 512;
static constexpr uint16_t kNMel = 32;

static std::vector<FrameOut> run_pipeline(const Fixture &fx) {
    std::vector<FrameOut> out;

    FFTProcessor<kFFTSize> fft(fx.sample_rate);
    MelFilterbank<kNMel, kFFTSize> mel;
    // freq_min=80 Hz matches production (audio_reactive.cpp) — the value was
    // raised from 40 in commit 2f91003 to align mel slot 0 with the realistic
    // low-end response of the supported mics. The test must use the same
    // value or it diverges from runtime behavior.
    mel.setup(fx.sample_rate, 80.0f, 16000.0f);
    MusicalBands bands;
    SuperFluxOnset<kNMel> onset;
    BTrack btrack;
    btrack.reset();

    // Rolling window of the most recent kFFTSize samples.
    std::vector<float> window(kFFTSize, 0.0f);

    const size_t N = fx.pcm.size();
    // Advance by hop, starting at the first hop that fills the window.
    // Pre-fill with zeros at the start so we still generate frames for the very
    // beginning of the clip (mirrors what the live stream does).
    size_t samples_written = 0;

    // Pre-compute how many frames we'll produce so we can reserve.
    const size_t expected_frames = N / kHopSize;
    out.reserve(expected_frames);

    float mel_frame[kNMel];
    float mag_sq[kFFTSize / 2];
    float bands_out[MusicalBands::kNumBands];

    for (size_t base = 0; base + kHopSize <= N; base += kHopSize) {
        // Shift window left by kHopSize and append the new hop's worth of samples.
        std::memmove(window.data(),
                     window.data() + kHopSize,
                     (kFFTSize - kHopSize) * sizeof(float));
        std::memcpy(window.data() + (kFFTSize - kHopSize),
                    fx.pcm.data() + base,
                    kHopSize * sizeof(float));
        samples_written += kHopSize;

        // FFT on the full window.
        fft.process(window.data());
        const float *mags = fft.magnitudes();
        for (size_t i = 0; i < kFFTSize / 2; i++) {
            mag_sq[i] = mags[i] * mags[i];
        }
        mel.process(mag_sq, mel_frame);
        bands.process(mel_frame, bands_out);
        auto sf = onset.process(mel_frame);
        auto bt = btrack.process(sf.strength);

        FrameOut f;
        f.bpm = bt.bpm;
        f.beat_phase = bt.beat_phase;
        f.beat_confidence = bt.confidence;
        f.beat_event = bt.beat_event;
        f.superflux_strength = sf.strength;
        f.superflux_event = sf.event;
        for (uint8_t m = 0; m < MusicalBands::kNumBands; m++) {
            f.bands[m] = bands_out[m];
            // Also compute raw per-band mel sum (matches what MusicalBands::process
            // sums internally before AGC) so we can diagnose under/over-amplification.
            float raw_sum = 0.0f;
            for (uint8_t i = MusicalBands::kMelStart[m]; i < MusicalBands::kMelEnd[m]; i++) {
                raw_sum += mel_frame[i];
            }
            f.raw_band_sum[m] = raw_sum;
        }
        out.push_back(f);
    }
    (void) samples_written;
    return out;
}

// ---- Metrics ---------------------------------------------------------------

struct Metrics {
    float avg_bpm_last_5s;
    int detected_onsets;
    int truth_onsets;
    int matched;
    float precision;
    float recall;
    float f_measure;
    // Per-band steady-state diagnostics, computed over the last half of frames.
    // band_mean[m]   = mean smoothed AGC output of band m
    // band_p95[m]    = 95th-percentile of smoothed AGC output of band m
    // raw_band_mean[m] = mean RAW mel sum (pre-AGC) so we can distinguish
    //                    AGC issues from "mel filter has no signal here".
    // The pair detects "AGC saturation": if band_p95[m] is near 1.0 AND
    // band_mean[m] is also near 1.0, the AGC failed to converge to target.
    float band_mean[MusicalBands::kNumBands];
    float band_p95[MusicalBands::kNumBands];
    float raw_band_mean[MusicalBands::kNumBands];
    // BPM spread over the last 10 s of frames. On a fixed-tempo fixture a
    // correct tracker holds a tight cluster around truth (near-zero range
    // AND near-zero variance); a hunting tracker sweeps a wide range. The
    // range (max - min) is the assertion input; the variance is kept as a
    // printed diagnostic. Stuck-at-wrong-value is caught by the separate
    // avg-BPM-vs-truth assertion, not by demanding variance.
    float bpm_variance_last_10s;
    float bpm_max_last_10s;
    float bpm_min_last_10s;
};

static Metrics compute_metrics(const Fixture &fx, const std::vector<FrameOut> &frames) {
    Metrics m{};
    // BPM convergence over last 5 seconds of frames.
    const float hop_sec = static_cast<float>(kHopSize) / fx.sample_rate;
    const size_t window_frames = static_cast<size_t>(5.0f / hop_sec);
    size_t start = frames.size() > window_frames ? frames.size() - window_frames : 0;
    double sum_bpm = 0.0;
    size_t count = 0;
    for (size_t i = start; i < frames.size(); i++) {
        sum_bpm += frames[i].bpm;
        count++;
    }
    m.avg_bpm_last_5s = count > 0 ? static_cast<float>(sum_bpm / count) : 0.0f;

    // Detected onset times from SuperFlux events.
    std::vector<float> detected;
    for (size_t i = 0; i < frames.size(); i++) {
        if (frames[i].superflux_event) {
            detected.push_back(static_cast<float>(i) * hop_sec);
        }
    }
    m.detected_onsets = static_cast<int>(detected.size());
    m.truth_onsets = static_cast<int>(fx.onset_times.size());

    // Greedy 1:1 matching with ±50 ms tolerance.
    const float tol = 0.050f;
    std::vector<bool> truth_used(fx.onset_times.size(), false);
    int matched = 0;
    for (float d : detected) {
        int best = -1;
        float best_err = tol;
        for (size_t j = 0; j < fx.onset_times.size(); j++) {
            if (truth_used[j]) continue;
            float err = std::fabs(d - fx.onset_times[j]);
            if (err <= best_err) {
                best_err = err;
                best = static_cast<int>(j);
            }
        }
        if (best >= 0) {
            truth_used[best] = true;
            matched++;
        }
    }
    m.matched = matched;
    m.precision = m.detected_onsets > 0 ? static_cast<float>(matched) / m.detected_onsets : 0.0f;
    m.recall = m.truth_onsets > 0 ? static_cast<float>(matched) / m.truth_onsets : 0.0f;
    if (m.precision + m.recall > 0.0f) {
        m.f_measure = 2.0f * m.precision * m.recall / (m.precision + m.recall);
    } else {
        m.f_measure = 0.0f;
    }

    // Per-band saturation metrics, computed over the LAST HALF of frames so
    // the per-band AGCs have had warmup time to settle.
    // (AGC time constants: release=1/6144 ≈ 71s. Half of a 60s fixture is
    // 30s — same order of magnitude, but enough for the integrator to clamp
    // and the gain to start its descent on saturating inputs.)
    // For a converged AGC at target=0.5, band_mean should hover near 0.5
    // with band_p95 below 1.0. Stuck-saturated AGCs sit near 1.0 for both.
    size_t band_start = frames.size() / 2;
    for (uint8_t b = 0; b < MusicalBands::kNumBands; b++) {
        std::vector<float> samples;
        samples.reserve(frames.size() - band_start);
        for (size_t i = band_start; i < frames.size(); i++) {
            samples.push_back(frames[i].bands[b]);
        }
        if (samples.empty()) {
            m.band_mean[b] = 0.0f;
            m.band_p95[b] = 0.0f;
            m.raw_band_mean[b] = 0.0f;
            continue;
        }
        double sum = 0.0;
        for (float v : samples) sum += v;
        m.band_mean[b] = static_cast<float>(sum / samples.size());
        std::sort(samples.begin(), samples.end());
        size_t idx = static_cast<size_t>(0.95 * (samples.size() - 1));
        m.band_p95[b] = samples[idx];

        // Raw-mel-sum mean over the same window for diagnostic.
        double raw_sum = 0.0;
        size_t raw_count = 0;
        for (size_t i = band_start; i < frames.size(); i++) {
            raw_sum += frames[i].raw_band_sum[b];
            raw_count++;
        }
        m.raw_band_mean[b] = raw_count > 0 ? static_cast<float>(raw_sum / raw_count) : 0.0f;
    }

    // BPM variance over the last 10 s of frames.
    const size_t bpm_window_frames = static_cast<size_t>(10.0f / hop_sec);
    size_t bpm_start = frames.size() > bpm_window_frames ? frames.size() - bpm_window_frames : 0;
    if (bpm_start < frames.size()) {
        double bsum = 0.0;
        size_t bcount = 0;
        float bmin = 1e9f, bmax = 0.0f;
        for (size_t i = bpm_start; i < frames.size(); i++) {
            float v = frames[i].bpm;
            bsum += v;
            bcount++;
            if (v < bmin) bmin = v;
            if (v > bmax) bmax = v;
        }
        float mean = bcount > 0 ? static_cast<float>(bsum / bcount) : 0.0f;
        double vsum = 0.0;
        for (size_t i = bpm_start; i < frames.size(); i++) {
            float d = frames[i].bpm - mean;
            vsum += d * d;
        }
        m.bpm_variance_last_10s = bcount > 0 ? static_cast<float>(vsum / bcount) : 0.0f;
        m.bpm_min_last_10s = bmin;
        m.bpm_max_last_10s = bmax;
    } else {
        m.bpm_variance_last_10s = 0.0f;
        m.bpm_min_last_10s = 0.0f;
        m.bpm_max_last_10s = 0.0f;
    }

    return m;
}

// ---- Per-fixture assertions ------------------------------------------------

static bool run_one(const std::string &stem) {
    Fixture fx;
    if (!load_fixture(stem, &fx)) {
        std::fprintf(stderr, "FAIL: %s (load failure)\n", stem.c_str());
        return false;
    }
    if (fx.sample_rate != 44100.0f) {
        std::fprintf(stderr, "FAIL: %s (expected 44.1 kHz, got %.1f)\n",
                     stem.c_str(), fx.sample_rate);
        return false;
    }
    auto frames = run_pipeline(fx);
    Metrics m = compute_metrics(fx, frames);

    const float bpm_err = std::fabs(m.avg_bpm_last_5s - fx.bpm);
    // ±3 BPM threshold per plan. Metronome fixtures lock tightly, but we use
    // the same threshold as the plan for the initial seed set.
    const float bpm_tol = 3.0f;
    // F-measure threshold per plan: 0.75 for well-behaved fixtures. Metronomes
    // with clean transients should easily clear 0.9.
    const float f_tol = 0.75f;

    bool bpm_ok = bpm_err <= bpm_tol;

    // Stress fixtures intentionally include sub-beat onsets (hi-hats etc.)
    // that the truth set does not enumerate — SuperFlux correctly detects
    // them, lowering precision. Use a relaxed F threshold for those.
    bool is_stress = stem.find("music_stress") != std::string::npos
                     || stem.find("music_") != std::string::npos;
    const float f_tol_local = is_stress ? 0.5f : f_tol;
    bool f_ok = m.f_measure >= f_tol_local;

    // Per-band assertions on stress fixtures only. Three failure modes:
    //  (a) saturation: p95 ≥ 0.97 + mean ≥ 0.85 — AGC stuck near clip.
    //  (b) under-amp:  p95 < 0.05 — band silent despite real signal.
    //  (c) BPM hunting: on a fixed-tempo fixture the last-10s BPM range must
    //      stay inside one grid neighbourhood. (The old variance-floor check
    //      asserted variance >= 0.5, which penalised CORRECT stable tracking.)
    const float band_p95_max = 0.97f;
    const float band_mean_max = 0.85f;
    const float band_p95_min = 0.05f;
    const float bpm_range_max = 6.0f;

    bool sat_ok = true;
    bool amp_ok = true;
    if (is_stress) {
        // Per-band saturation check: at least one band saturating fails.
        for (uint8_t b = 0; b < MusicalBands::kNumBands; b++) {
            if (m.band_p95[b] > band_p95_max && m.band_mean[b] > band_mean_max) {
                sat_ok = false;
            }
        }
        // Per-band under-amplification: ALL bands silent fails. Per-band
        // failure (e.g. air band silent on a fixture with no high freq) is OK.
        bool any_active = false;
        for (uint8_t b = 0; b < MusicalBands::kNumBands; b++) {
            if (m.band_p95[b] >= band_p95_min) {
                any_active = true;
                break;
            }
        }
        amp_ok = any_active;
    }
    bool var_ok = !is_stress ||
                  ((m.bpm_max_last_10s - m.bpm_min_last_10s) <= bpm_range_max);

    std::printf(
        "  %-24s frames=%zu  bpm=%.2f (truth=%.0f, err=%.2f, %s)  "
        "F=%.3f (P=%.3f R=%.3f, det=%d truth=%d, %s)\n",
        stem.c_str(),
        frames.size(),
        static_cast<double>(m.avg_bpm_last_5s),
        static_cast<double>(fx.bpm),
        static_cast<double>(bpm_err),
        bpm_ok ? "OK" : "FAIL",
        static_cast<double>(m.f_measure),
        static_cast<double>(m.precision),
        static_cast<double>(m.recall),
        m.detected_onsets,
        m.truth_onsets,
        f_ok ? "OK" : "FAIL");

    if (is_stress) {
        const char *bands_status = sat_ok ? (amp_ok ? "OK" : "FAIL (under-amp)")
                                          : "FAIL (saturation)";
        std::printf(
            "    bands (mean / p95):  "
            "low_bass=%.2f/%.2f  bass=%.2f/%.2f  low_mid=%.2f/%.2f  "
            "mid=%.2f/%.2f  upper_mid=%.2f/%.2f  high=%.2f/%.2f  "
            "air=%.2f/%.2f  [%s]\n",
            (double) m.band_mean[0], (double) m.band_p95[0],
            (double) m.band_mean[1], (double) m.band_p95[1],
            (double) m.band_mean[2], (double) m.band_p95[2],
            (double) m.band_mean[3], (double) m.band_p95[3],
            (double) m.band_mean[4], (double) m.band_p95[4],
            (double) m.band_mean[5], (double) m.band_p95[5],
            (double) m.band_mean[6], (double) m.band_p95[6],
            bands_status);
        std::printf(
            "    raw mel sums (mean): "
            "low_bass=%.4f  bass=%.4f  low_mid=%.4f  mid=%.4f  "
            "upper_mid=%.4f  high=%.4f  air=%.4f\n",
            (double) m.raw_band_mean[0], (double) m.raw_band_mean[1],
            (double) m.raw_band_mean[2], (double) m.raw_band_mean[3],
            (double) m.raw_band_mean[4], (double) m.raw_band_mean[5],
            (double) m.raw_band_mean[6]);
        std::printf(
            "    bpm last 10s: range=[%.1f, %.1f] (spread=%.1f, max %.1f)  "
            "variance=%.2f (diagnostic)  [%s]\n",
            (double) m.bpm_min_last_10s, (double) m.bpm_max_last_10s,
            (double) (m.bpm_max_last_10s - m.bpm_min_last_10s),
            (double) bpm_range_max,
            (double) m.bpm_variance_last_10s,
            var_ok ? "OK" : "FAIL (BPM hunting)");
    }

    return bpm_ok && f_ok && sat_ok && amp_ok && var_ok;
}

int main() {
    const std::string dir = fixture_dir();
    auto wavs = list_wavs(dir);
    if (wavs.empty()) {
        std::fprintf(stderr, "ERROR: no WAV fixtures found in %s\n", dir.c_str());
        return 1;
    }
    std::printf("E2E pro pipeline: %zu fixture(s) in %s\n", wavs.size(), dir.c_str());

    // All fixture assertions (including the stress fixtures' band and BPM
    // checks) are fatal. The stress assertions were diagnostic-only while
    // the AGC / BPM-stuck bugs from docs/plans/audio-pro-dsp-fixes-audit.md
    // were open; those fixes have landed, so any failure here is a real
    // regression.
    int fails = 0;
    for (const auto &wav : wavs) {
        std::string stem = wav.substr(0, wav.size() - 4);
        bool ok = run_one(stem);
        if (!ok) {
            fails++;
        }
    }
    if (fails == 0) {
        std::printf("All %zu fixture(s) passed required assertions.\n", wavs.size());
        return 0;
    }
    std::fprintf(stderr, "%d fixture(s) failed.\n", fails);
    return 1;
}
