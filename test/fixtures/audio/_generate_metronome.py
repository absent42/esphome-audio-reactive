#!/usr/bin/env python3
"""Generate a metronome-click WAV + JSON annotation for pipeline tests.

See README.md in this directory for the fixture format and license rules.

Example:
    python3 _generate_metronome.py --bpm 120 --seconds 15
    python3 _generate_metronome.py --bpm 90  --seconds 15

Output files land in the current directory: `metronome_<bpm>bpm.wav` and
`metronome_<bpm>bpm.json`. The script prefers `soundfile`; if it is not
installed it falls back to `scipy.io.wavfile`, then to a manual PCM-16 WAV
writer so it works inside minimal container environments.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

import numpy as np


def synthesize(bpm: int, seconds: float, sample_rate: int) -> tuple[np.ndarray, list[float]]:
    """Return (float32 audio in [-1, 1], onset_times_in_seconds)."""
    n_samples = int(sample_rate * seconds)
    period_samples = int(round(sample_rate * 60.0 / bpm))
    # Click: short 1 kHz sine with a fast exponential decay envelope.
    click_ms = 10.0
    click_samples = int(sample_rate * click_ms / 1000.0)
    t = np.arange(click_samples) / sample_rate
    click = (np.sin(2.0 * np.pi * 1000.0 * t) * np.exp(-t * 200.0)).astype(np.float32)
    # Scale so peaks stay well below full-scale after accumulation (leaves
    # headroom for the 16-bit conversion below).
    click *= 0.6

    audio = np.zeros(n_samples, dtype=np.float32)
    onset_times: list[float] = []
    start = 0
    while start + click_samples <= n_samples:
        audio[start : start + click_samples] += click
        onset_times.append(start / sample_rate)
        start += period_samples

    # Hard-clip protection (should never trigger at 0.6 amplitude).
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

    # Manual fallback: write a minimal RIFF/WAVE PCM16 mono file.
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
    ap.add_argument("--bpm", type=int, required=True, help="Beats per minute (integer)")
    ap.add_argument("--seconds", type=float, default=15.0, help="Clip length (default 15s)")
    ap.add_argument(
        "--sample-rate", type=int, default=44100, help="Sample rate in Hz (default 44100)"
    )
    ap.add_argument(
        "--out-dir",
        type=Path,
        default=Path(__file__).resolve().parent,
        help="Output directory (default: this script's directory)",
    )
    args = ap.parse_args()

    if args.bpm <= 0:
        print("bpm must be positive", file=sys.stderr)
        return 2
    if args.seconds <= 0:
        print("seconds must be positive", file=sys.stderr)
        return 2
    if args.sample_rate != 44100:
        print(
            "warning: sample_rate != 44100; the E2E pipeline expects 44.1 kHz fixtures",
            file=sys.stderr,
        )

    audio, onset_times = synthesize(args.bpm, args.seconds, args.sample_rate)
    stem = f"metronome_{args.bpm}bpm"
    wav_path = args.out_dir / f"{stem}.wav"
    json_path = args.out_dir / f"{stem}.json"

    write_wav(wav_path, audio, args.sample_rate)
    annotation = {
        "bpm": args.bpm,
        "onset_times": [round(t, 6) for t in onset_times],
        "band_profile": "transient-only broadband click (1 kHz sine with 5 ms decay)",
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
