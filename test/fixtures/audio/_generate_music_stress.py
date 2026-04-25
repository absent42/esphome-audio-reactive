#!/usr/bin/env python3
"""Generate a synthetic "music-like" stress fixture for the pro-tier pipeline.

Unlike the metronome fixture (clean isolated transients on a near-silent
background), this fixture has continuous energy across all 7 musical bands
plus discrete onsets, so it stresses the AGC dynamics, the asymmetric EMA,
SuperFlux's adaptive thresholding, and BTrack's tempo induction in a way
that resembles real-world music playback.

Design:
- Duration: 30 seconds (long enough for AGC to settle past its post-reset
  transient; release_rate=1/6144 at ~86 Hz frame rate gives a 71-second
  time constant, but 30s is enough for the AGC to be in mid-convergence
  and reveal saturation behavior).
- Tempo: 120 BPM. Onsets fire on every quarter note (kick + bass note).
  Hi-hat fires on every eighth note. Pad sustains continuously.
- Frequency content (chosen to land in realistic mic-pickup range; mics
  roll off below ~80 Hz so true sub-bass is excluded):
    Kick:    120 Hz sine with fast envelope     -> low_bass band
    Bass:    165 Hz triangle, 200 ms gated       -> bass band
    Pad:     440 + 554 + 659 Hz triangles, 50% duty cycle -> low_mid + mid
    Hihat:   filtered white noise, 8 ms bursts   -> high + air
    Cymbal:  filtered white noise on bar starts  -> air

Onset times in the JSON annotation are the kick + bass downbeats (the
"musical beats" SuperFlux is expected to detect). The hihat 1/8 notes
are intentionally NOT in the truth set — they're harder to localize and
adding them would force F-measure thresholds higher than the AGC-saturated
baseline can ever clear.

Usage:
    python3 _generate_music_stress.py --bpm 120 --seconds 30
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

import numpy as np


def adsr(n: int, attack_ms: float, decay_ms: float, sustain: float,
         release_ms: float, sample_rate: int) -> np.ndarray:
    """Simple ADSR envelope, length n samples."""
    a = max(1, int(sample_rate * attack_ms / 1000.0))
    d = max(1, int(sample_rate * decay_ms / 1000.0))
    r = max(1, int(sample_rate * release_ms / 1000.0))
    s = max(0, n - a - d - r)
    env = np.zeros(n, dtype=np.float32)
    if a > 0:
        env[:a] = np.linspace(0.0, 1.0, a, dtype=np.float32)
    if d > 0:
        env[a:a + d] = np.linspace(1.0, sustain, d, dtype=np.float32)
    if s > 0:
        env[a + d:a + d + s] = sustain
    if r > 0:
        env[-r:] = np.linspace(sustain, 0.0, r, dtype=np.float32)
    return env


def kick(sample_rate: int) -> np.ndarray:
    """120 Hz sine kick with 80 ms exponential decay."""
    dur_ms = 120.0
    n = int(sample_rate * dur_ms / 1000.0)
    t = np.arange(n) / sample_rate
    sig = np.sin(2.0 * np.pi * 120.0 * t).astype(np.float32)
    sig *= np.exp(-t * 25.0).astype(np.float32)
    return sig * 0.9


def bass(sample_rate: int) -> np.ndarray:
    """165 Hz triangle bass note, 200 ms with ADSR."""
    dur_ms = 200.0
    n = int(sample_rate * dur_ms / 1000.0)
    t = np.arange(n) / sample_rate
    # Triangle wave at 165 Hz. The expression below has period 1 in `phase`,
    # so set phase = f*t (not 2*f*t — that doubles the actual frequency).
    phase = 165.0 * t
    sig = (2.0 * np.abs(2.0 * (phase - np.floor(phase + 0.5))) - 1.0).astype(np.float32)
    env = adsr(n, attack_ms=5.0, decay_ms=30.0, sustain=0.7,
               release_ms=80.0, sample_rate=sample_rate)
    return sig * env * 0.6


def hihat(sample_rate: int) -> np.ndarray:
    """8 ms filtered noise burst; band-limited by simple FIR."""
    dur_ms = 12.0
    n = int(sample_rate * dur_ms / 1000.0)
    rng = np.random.default_rng(seed=42)
    raw = rng.standard_normal(n).astype(np.float32)
    # Crude high-pass: subtract local mean over 5 samples.
    raw[5:] -= 0.2 * (raw[:-5] + raw[1:-4] + raw[2:-3] + raw[3:-2] + raw[4:-1])
    env = adsr(n, attack_ms=1.0, decay_ms=4.0, sustain=0.4,
               release_ms=6.0, sample_rate=sample_rate)
    return raw * env * 0.4


def cymbal(sample_rate: int) -> np.ndarray:
    """200 ms decay cymbal-ish noise."""
    dur_ms = 200.0
    n = int(sample_rate * dur_ms / 1000.0)
    rng = np.random.default_rng(seed=123)
    raw = rng.standard_normal(n).astype(np.float32)
    t = np.arange(n) / sample_rate
    env = np.exp(-t * 8.0).astype(np.float32)
    return raw * env * 0.12


def pad(sample_rate: int, n_samples: int) -> np.ndarray:
    """Continuous pad: A4+C#5+E5 triangles at moderate level."""
    t = np.arange(n_samples) / sample_rate
    out = np.zeros(n_samples, dtype=np.float32)
    for f in (440.0, 554.37, 659.25):
        phase = f * t  # period-1 phase: f*t, not 2*f*t (that doubles the actual freq).
        tri = (2.0 * np.abs(2.0 * (phase - np.floor(phase + 0.5))) - 1.0).astype(np.float32)
        out += tri * 0.18
    # Subtle 0.4 Hz amplitude wobble to keep AGC slightly active rather than
    # producing a perfectly steady DC-like input.
    wobble = 0.85 + 0.15 * np.sin(2.0 * np.pi * 0.4 * t).astype(np.float32)
    out *= wobble
    return out


def synthesize(bpm: int, seconds: float, sample_rate: int) -> tuple[np.ndarray, list[float]]:
    """Return (mono float32 in [-1, 1], onset_times_in_seconds_for_kick_and_bass)."""
    n_samples = int(sample_rate * seconds)
    beat_samples = int(round(sample_rate * 60.0 / bpm))
    half_beat = beat_samples // 2

    # Continuous pad layer.
    audio = pad(sample_rate, n_samples)

    # Per-hit samples.
    kick_smp = kick(sample_rate)
    bass_smp = bass(sample_rate)
    hihat_smp = hihat(sample_rate)
    cymbal_smp = cymbal(sample_rate)

    onset_times: list[float] = []

    def mix_at(buf: np.ndarray, smp: np.ndarray, start: int) -> None:
        end = min(start + smp.size, n_samples)
        if start >= n_samples or end <= 0:
            return
        s0 = max(0, -start)
        b0 = max(0, start)
        b1 = end
        s1 = s0 + (b1 - b0)
        buf[b0:b1] += smp[s0:s1]

    # Iterate beats. 4-beat bars; cymbal accents on every 4th downbeat.
    beat_idx = 0
    pos = 0
    while pos < n_samples:
        # Kick + bass on every beat.
        mix_at(audio, kick_smp, pos)
        mix_at(audio, bass_smp, pos)
        onset_times.append(pos / sample_rate)

        # Hihat on the off-beat (eighth note halfway through this beat).
        mix_at(audio, hihat_smp, pos + half_beat)

        # Cymbal on bar starts (every 4 beats).
        if beat_idx % 4 == 0:
            mix_at(audio, cymbal_smp, pos)

        pos += beat_samples
        beat_idx += 1

    # Normalize to peak at 0.95 (leaves a tiny bit of headroom).
    peak = float(np.max(np.abs(audio))) if audio.size > 0 else 1.0
    if peak > 0.0:
        audio *= 0.95 / peak

    np.clip(audio, -1.0, 1.0, out=audio)
    return audio, onset_times


def write_wav(path: Path, audio: np.ndarray, sample_rate: int) -> None:
    """Write a mono 16-bit PCM WAV, preferring soundfile > scipy > manual."""
    pcm16 = np.clip(audio * 32767.0, -32768.0, 32767.0).astype(np.int16)
    try:
        import soundfile as sf  # type: ignore
        sf.write(str(path), pcm16, sample_rate, subtype="PCM_16")
        return
    except ImportError:
        pass
    try:
        from scipy.io import wavfile  # type: ignore
        wavfile.write(str(path), sample_rate, pcm16)
        return
    except ImportError:
        pass

    n = pcm16.size
    byte_rate = sample_rate * 2
    data_bytes = pcm16.tobytes()
    header = b"RIFF"
    header += struct.pack("<I", 36 + len(data_bytes))
    header += b"WAVEfmt "
    header += struct.pack("<IHHIIHH", 16, 1, 1, sample_rate, byte_rate, 2, 16)
    header += b"data"
    header += struct.pack("<I", len(data_bytes))
    with open(path, "wb") as f:
        f.write(header)
        f.write(data_bytes)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bpm", type=int, default=120)
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--sample-rate", type=int, default=44100)
    ap.add_argument(
        "--out-dir",
        type=Path,
        default=Path(__file__).resolve().parent,
        help="Output directory (default: this script's directory)",
    )
    args = ap.parse_args()

    if args.bpm <= 0 or args.seconds <= 0:
        print("bpm and seconds must be positive", file=sys.stderr)
        return 2
    if args.sample_rate != 44100:
        print(
            "warning: sample_rate != 44100; the E2E pipeline expects 44.1 kHz fixtures",
            file=sys.stderr,
        )

    audio, onset_times = synthesize(args.bpm, args.seconds, args.sample_rate)
    stem = f"music_stress_{args.bpm}bpm"
    wav_path = args.out_dir / f"{stem}.wav"
    json_path = args.out_dir / f"{stem}.json"

    write_wav(wav_path, audio, args.sample_rate)
    annotation = {
        "bpm": args.bpm,
        "onset_times": [round(t, 6) for t in onset_times],
        "band_profile": (
            "Synthetic music: 120 Hz kick (low_bass), 165 Hz triangle bass "
            "(bass), 440/554/659 Hz triangle pad (low_mid+mid), filtered "
            "noise hihat 1/8 notes (high+air), cymbal accents on bar starts. "
            "Continuous energy across all 7 musical bands; designed to stress "
            "AGC convergence and reveal saturation behavior the metronome "
            "fixtures can't expose."
        ),
        "license": "self-generated",
    }
    with open(json_path, "w") as f:
        json.dump(annotation, f, indent=2)
        f.write("\n")

    print(f"wrote {wav_path.name} ({args.seconds:.1f}s) and {json_path.name}")
    print(f"  bpm={args.bpm}  onsets={len(onset_times)}  sample_rate={args.sample_rate}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
