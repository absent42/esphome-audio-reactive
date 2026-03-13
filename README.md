# esphome-audio-reactive

ESP32 audio analysis component for ESPHome. Provides real-time beat detection,
frequency band energy, amplitude, and BPM estimation via on-device FFT processing.

Designed as the audio source for
[Aqara Advanced Lighting](https://github.com/absent42/aqara-advanced-lighting)
audio-reactive dynamic scenes, but usable with any Home Assistant automation.

## Hardware

Any ESP32 with an I2S digital microphone:

| Device | Price | Notes |
|--------|-------|-------|
| M5Stack ATOM Echo | ~$13 | Primary tested device, built-in PDM mic |
| ESP32 + ICS-43434 | ~$8 | Best mic accuracy, 4-wire DIY |

## Installation

### One-click install (recommended)

Visit **[absent42.github.io/esphome-audio-reactive](https://absent42.github.io/esphome-audio-reactive/)**
and click Install. Connect your ESP32 via USB — no ESPHome knowledge required.

### Manual ESPHome setup

If you prefer to compile yourself or need custom configuration:

Add to your ESPHome YAML:

    external_components:
      - source: github://absent42/esphome-audio-reactive
        components: [audio_reactive]

See `atom-echo.yaml` for a complete config.

## Exposed Entities

| Entity | Type | Update Rate | Description |
|--------|------|-------------|-------------|
| Beat Detected | binary_sensor | On event | Pulses on when a beat is detected |
| Bass Energy | sensor (0-1) | ~50ms | Normalized bass band energy (20-350 Hz) |
| Mid Energy | sensor (0-1) | ~50ms | Normalized mid band energy (350-2000 Hz) |
| High Energy | sensor (0-1) | ~50ms | Normalized high band energy (2-5 kHz) |
| Amplitude | sensor (0-1) | ~50ms | Overall normalized amplitude |
| BPM | sensor | ~3s | Estimated beats per minute |

## Configuration

    audio_reactive:
      microphone: mic_id          # Required: I2S microphone component ID
      update_interval: 50ms       # Processing interval (default: 50ms)
      beat_sensitivity: 50        # 1-100, higher = reacts to quieter beats (default: 50)

## How It Works

1. I2S microphone captures audio at ~10kHz sample rate
2. 512-sample FFT (ArduinoFFT) produces frequency bin magnitudes
3. Bins grouped into bass (20-350 Hz), mid (350-2kHz), high (2-5kHz) bands
4. AGC normalizes energy values to 0-1 against rolling ambient level
5. Beat detector compares bass energy to rolling threshold; tracks BPM from intervals

## License

MIT
