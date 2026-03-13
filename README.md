# ESPHome Audio Reactive

ESP32 audio analysis component for ESPHome. Provides real-time beat detection,
frequency band energy, amplitude, and BPM estimation via on-device FFT processing.

Designed as the audio source for
[Aqara Advanced Lighting](https://github.com/absent42/aqara-advanced-lighting)
audio-reactive dynamic scenes, but usable with any Home Assistant automation.

Produces Home Assistant sensors for Beat Detected, BPM, Bass Frequency Energy , Mid Frequency Engergy, High Frequency Energy, Amplitude, and a beat detection sensitivity parameter.

## Hardware

Can be adpated for any ESP32 with an I2S digital microphone. Builds currently available for:

| Device | Price | Notes |
|--------|-------|-------|
| M5Stack ATOM Echo | ~$13 | Built-in PDM mic |

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

### Security (recommended)

After installing, add API encryption and an OTA password to your device config.
If you use the ESPHome Dashboard (Home Assistant add-on), it generates encryption
keys automatically when you add the `encryption:` block.

Add these to your YAML:

    api:
      encryption:
        key: !secret api_encryption_key

    ota:
      - platform: esphome
        password: !secret ota_password

Then add matching values to your `secrets.yaml` (managed by the ESPHome Dashboard,
or created manually alongside your device YAML).

API encryption secures communication between the device and Home Assistant.
The OTA password prevents unauthorized over-the-network firmware updates.
Neither affects the USB web installer.

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

## How It Works

1. I2S microphone captures audio at 16kHz sample rate
2. 512-sample FFT (ArduinoFFT) produces frequency bin magnitudes
3. Bins grouped into bass (20-350 Hz), mid (350-2kHz), high (2-5kHz) bands
4. AGC normalizes energy values to 0-1 against rolling ambient level
5. Beat detector compares bass energy to rolling threshold; tracks BPM from intervals

## License

MIT
