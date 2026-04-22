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
};

static constexpr size_t kFFTSize = 2048;
static constexpr size_t kHopSize = 512;
static constexpr uint16_t kNMel = 32;

static std::vector<FrameOut> run_pipeline(const Fixture &fx) {
    std::vector<FrameOut> out;

    FFTProcessor<kFFTSize> fft(fx.sample_rate);
    MelFilterbank<kNMel, kFFTSize> mel;
    mel.setup(fx.sample_rate, 40.0f, 16000.0f);
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
    bool f_ok = m.f_measure >= f_tol;

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
    return bpm_ok && f_ok;
}

int main() {
    const std::string dir = fixture_dir();
    auto wavs = list_wavs(dir);
    if (wavs.empty()) {
        std::fprintf(stderr, "ERROR: no WAV fixtures found in %s\n", dir.c_str());
        return 1;
    }
    std::printf("E2E pro pipeline: %zu fixture(s) in %s\n", wavs.size(), dir.c_str());

    int fails = 0;
    for (const auto &wav : wavs) {
        // Strip ".wav" suffix to get the stem used for both files.
        std::string stem = wav.substr(0, wav.size() - 4);
        if (!run_one(stem)) fails++;
    }
    if (fails == 0) {
        std::printf("All %zu fixture(s) passed.\n", wavs.size());
        return 0;
    }
    std::fprintf(stderr, "%d fixture(s) failed.\n", fails);
    return 1;
}
