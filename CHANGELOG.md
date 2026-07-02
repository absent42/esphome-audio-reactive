# Changelog

All notable changes to the ESPHome Audio Reactive component will be documented in this file.

## [0.5.0] - 2026-07-02

### Changed

- Rewrote BTrack tempo induction: fractional-lag harmonic-template scoring
  over a continuous 60-180 BPM grid (new `TempoEstimator` class) replaces the
  integer-lag comb filterbank and Viterbi prior. Fixes the BPM sensor
  sticking at 114/150, the systematic -2 to -6 BPM bias, and confident BPM
  readings on non-musical audio.
- BPM detection range widened from 80-160 to 60-180.
- BPM confidence is now evidence-based: the peak-mass fraction of the
  smoothed tempo distribution, gated on argmax stability. Steady music
  typically reads around 0.5; the sensor reports 0 during the ~3 s warmup,
  on non-rhythmic audio (speech, TV, ambient noise), and during tempo
  transitions. A song change without a silence gap relocks within ~15 s.
- Known limitation: above ~160 BPM with strong eighth-note content the
  tracker can lock on the 2/3 (or 1/2) sub-harmonic at marginal confidence
  (measured 168 -> 112 at confidence 0.33). A half-lag template term was
  tried and reverted because it introduced half-tempo locks on eighth-free
  material; see the KNOWN LIMITATION note in
  `components/audio_reactive/tempo_estimator.h`.

### Added

- Ring-buffer overflow counter in the audio debug dump (`dropped=` field in
  the ring-buffer log line) for diagnosing on-device sample loss; periodic
  drops inject onset discontinuities that can masquerade as beats.

## [0.4.2] - 2026-04-27

### Fixed (pro tier)

- BTrack beat tracker rewritten from scratch against the adamstark/BTrack
  reference. The previous port had accumulated enough drift through six
  successive patches that on real music the BPM locked at ~114 regardless
  of tempo. The new implementation now reports the correct BPM across the
  full 80-160 detection range — verified with metronome fixtures at 90 BPM
  and 120 BPM, and a synthetic music-stress fixture at 120 BPM. Three
  load-bearing fixes that landed in the rewrite: (a) adaptive thresholding
  is applied to the onset detection function BEFORE the autocorrelation
  step, removing the DC bias that was burying beat-period peaks in the ACF;
  (b) the initial Viterbi prior is uniform 1.0 across all candidates instead
  of a tightly-peaked Gaussian centred on 120 BPM, so off-prior tempos can
  lock without being pulled toward the prior; (c) tempo induction now runs
  per-beat (when the beat-prediction timer hits zero) rather than every 43
  frames on a fixed cadence, matching the reference's event-driven design.
  The full re-derivation lives in [btrack.h](components/audio_reactive/btrack.h),
  driven test-first by 14 unit tests in `test/test_btrack/test_btrack.cpp`.
- Confidence-weighted soft-blend Viterbi state update (deviation from the
  reference, added after the rewrite locked correctly at 140 BPM on a
  146-BPM song but drifted to 114 BPM after a single low-onset-strength
  frame). The reference's `calculateTempo()` unconditionally replaces
  `prev_delta` with the new delta on every step. On real-time mic-captured
  audio that lets one noisy frame corrupt the prior and build a "wrong"
  lock that survives even when good observations resume. The opposite
  extreme — a hard gate that rejects low-confidence updates — also fails
  because it breaks the bootstrap feedback loop the algorithm relies on
  to amplify mid-confidence locks into stable ones. The shipped fix is a
  soft blend: each frame updates `prev_delta_ ← conf · delta_ + (1 - conf)
  · prev_delta_`, with `conf` clamped to a small floor (0.03) so a real
  tempo change still wins eventually. High-confidence frames behave like
  the reference; low-confidence transients only nudge the prior by a
  small fraction; the bootstrap loop continues to work because every
  frame contributes proportional to its own confidence.

## [0.4.1] - 2026-04-25

### Fixed (pro tier)

- Quiet-room calibration now measures per-musical-band noise floors directly
  from the raw mel pipeline. The previous V1->V2 interpolation stopgap put V2
  floors in the wrong scale relative to the AGC's input domain, causing the
  sub_bass / low_mid / upper_mid sensors to either saturate at 1.0 or stick
  near 0 even after a fresh calibration. V1->V2 interpolation is now reserved
  as the boot-time legacy fallback only.
- BTrack BPM no longer sticks at a stale value through silence. `current_bpm_`
  resets to the 120 BPM prior on prolonged zero-onset windows so the next
  non-silent window starts tempo induction without a previous lock biasing
  the comb-filterbank weighting. The BPM sensor publish is also gated on
  `bt_confidence >= kSilenceConfidence`; below threshold it publishes 0.0 to
  honestly mark "no tempo lock" instead of exposing the held internal value.

### Changed

- Mel filterbank `freq_min` raised from 40 Hz to 80 Hz on the pro tier. The
  PDM mics in basic-tier devices roll off below ~100 Hz, and the I2S codec
  mics in pro-tier devices typically have a 50-100 Hz HPF, so the lower
  portion of the previous range was mostly AC line hum and mic self-noise
  rather than musical content. With `freq_min=80`, mel slot 0 now spans
  ~80-240 Hz with peak sensitivity near 156 Hz - a realistic kick-drum /
  bass-guitar fundamental range.
- The first musical band's user-facing label has been renamed from "Sub Bass"
  to "Low Bass" to reflect the actual coverage. The YAML config key
  (`sub_bass_energy`), the C++ enum (`MusicalBands::SUB_BASS`), and the
  HA-integration discovery suffix (`_sub_bass_energy`) are unchanged - existing
  installs continue to work without YAML edits. Update the `name:` field in
  your YAML if you want the new label to apply.

## [0.4.0] - 2026-04-22

Version 0.4.0 introduces a two-tier DSP architecture. Classic ESP32 boards
(ATOM Echo, M5StickC Plus2) continue to run the existing pipeline unchanged,
while ESP32-S3 boards with PSRAM (ATOM Echo S3R, Waveshare ESP32-S3 Audio) now
run a richer "pro" pipeline with a 2048-point FFT at 44.1kHz, a 32-band log-mel
filterbank aggregated into 7 musical bands, SuperFlux onset detection, and the
full BTrack dynamic-programming beat tracker.

### Added (pro tier - ESP32-S3 + PSRAM)

- `dsp_tier: auto | basic | pro` YAML config key (default `auto`). Auto resolves
  to `pro` on ESP32-S3 boards that declare a top-level `psram:` block, and
  `basic` everywhere else. Explicit `pro` on non-S3 / no-PSRAM hardware fails
  validation at codegen time with a clear error.
- Pro-tier DSP pipeline: 2048-point FFT at 44.1kHz, 32-band log-mel filterbank,
  7-band musical aggregation (sub_bass, bass, low_mid, mid, upper_mid, high,
  air), SuperFlux spectral-flux onset detector with per-band median suppression,
  and the full adamstark/BTrack beat tracker (comb-filterbank tempo induction +
  Viterbi state filter + dynamic-programming beat prediction).
- Pro-only sensor platforms: `sub_bass_energy`, `low_mid_energy`,
  `upper_mid_energy`, `air_energy`. Each is a 0.0-1.0 normalized energy value
  aggregated from the log-mel filterbank.
- Pro-only binary sensors: `beat_event` (pulses 30ms on every BTrack beat
  boundary, useful for driving beat-predictive lighting without polling BPM)
  and `calibration_stale` (diagnostic - on when calibration data is missing or
  was migrated from a V1 store on first boot).
- Both tiers: opt-in debug diagnostic sensors `fft_task_cycle_mean_us` and
  `fft_task_cycle_peak_us`. When declared they report runtime performance
  telemetry (microseconds per FFT frame, measured inside the task excluding
  blocking waits); when omitted, all instrumentation is compiled out for zero
  runtime overhead.
- V2 calibration struct with per-musical-band noise floors. V1 calibration
  stores are migrated automatically on first boot by linearly interpolating
  the legacy three-band (bass/mid/high) noise floors into the seven musical
  bands. `calibration_stale` flips on to prompt the user to re-run
  quiet-room and music-level calibration for best results.
- Microphone sample-rate validation at codegen. The component now fails
  compilation with an actionable error if `microphone.sample_rate` does not
  match the resolved tier (22050 for basic, 44100 for pro).

### Changed

- Classic ESP32 hardware (ATOM Echo, M5StickC Plus2) continues to run the
  existing basic pipeline with no behavioural changes.
- S3/PSRAM boards auto-upgrade to the pro pipeline on flash. Existing V1
  calibration data migrates with linear interpolation and the
  `calibration_stale` binary sensor flips on to prompt re-calibration for
  best results.
- `audio_reactive.sample_rate` YAML key is removed: sample rate is now derived
  from the resolved tier. Users who pinned it explicitly must delete the line
  (the tier default applies automatically).

### Breaking (YAML only)

- `audio_reactive.sample_rate` is no longer a valid config key. If you had set
  it explicitly, remove the line - sample rate now follows the resolved DSP
  tier. No runtime behaviour changes for users who hadn't overridden the
  default.
