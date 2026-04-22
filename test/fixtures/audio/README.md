# Audio Fixtures for Pro-Tier Pipeline Tests

Fixtures in this directory feed the end-to-end pipeline test
(`test/test_e2e_pro_pipeline/`). Each clip is a paired WAV + JSON.

## Format

- `<clip>.wav`: **mono, 44.1 kHz, 16-bit PCM, <= 30 seconds** preferred (shorter
  is fine). Stereo or other sample rates will be rejected by the loader.
- `<clip>.json`:
  - `bpm` (int) - ground-truth tempo in beats per minute.
  - `onset_times` (array of floats) - onset times in seconds for a
    representative section (ideally the whole clip for short fixtures).
  - `band_profile` (free-form string) - notes on expected per-band energy
    content (e.g. "transient-only broadband click", "bass-heavy kick drum").
  - `license` (string) - one of the acceptable licenses below.

## Acceptable licenses

- `self-generated` or `self-generated (owner: <name>)` - programmatically
  produced (e.g. `_generate_metronome.py`) or self-recorded.
- `public-domain-musopen` - https://musopen.org (filtered to PD/CC0 only).
- `public-domain-freepd` - https://freepd.com (CC0).
- `public-domain-archive` - https://archive.org (filtered to public domain).
- `public-domain-us-gov` - US government recordings (PD by default; e.g. NASA).

## NOT acceptable

- "royalty-free" YouTube / stock-music clips. Royalty-free is not the same as
  public-domain; the underlying license typically restricts redistribution.
- Research datasets (GTZAN, MIREX, Ballroom, etc.) with unclear or
  non-commercial-only licensing.
- Any clip you cannot point at a specific PD / CC0 source for.

## Regenerating the metronome fixtures

```bash
cd test/fixtures/audio
python3 _generate_metronome.py --bpm 120 --seconds 15
python3 _generate_metronome.py --bpm 90 --seconds 15
```

See `_generate_metronome.py --help` for full options.
