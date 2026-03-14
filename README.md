# ESPHome Audio Reactive

ESP32 audio analysis component for ESPHome. Provides real-time onset/beat detection,
frequency band energy, amplitude, BPM estimation, and silence detection via on-device
FFT processing with a dedicated FreeRTOS task.

Designed as the audio source for
[Aqara Advanced Lighting](https://github.com/absent42/aqara-advanced-lighting)
audio-reactive dynamic scenes, but usable with any Home Assistant automation.

## Hardware

Can be adapted for any ESP32 with an I2S digital microphone. Configurations currently available for:

| Device | Price | Notes |
|--------|-------|-------|
| M5Stack ATOM Echo | ~$13 | Built-in PDM mic, status LED |

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
| Audio Sensor | binary_sensor | On event | Pulses on when a musical onset is detected (beat, cymbal, vocal entrance, etc.) |
| Silence | binary_sensor | On change | On when the environment is quiet (noise gate active for >1 second) |
| Bass Energy | sensor (0-1) | ~50ms | Smoothed, AGC-normalized bass band energy |
| Mid Energy | sensor (0-1) | ~50ms | Smoothed, AGC-normalized mid band energy |
| High Energy | sensor (0-1) | ~50ms | Smoothed, AGC-normalized high band energy |
| Amplitude | sensor (0-1) | ~50ms | Overall smoothed amplitude with dynamics limiting |
| BPM | sensor | ~3s | Estimated beats per minute (with confidence attribute) |
| Beat Sensitivity | number (1-100) | On change | Controls onset detection threshold |
| Squelch | number (0-100) | On change | Noise gate threshold (higher = requires louder signal) |
| Detection Mode | select | On change | `spectral_flux` (all genres) or `bass_energy` (rhythmic music) |
| Microphone Mute | switch | On change | Mute/unmute the microphone (LED turns red when muted) |
| Reset AGC | button | On press | Resets automatic gain control and onset detector |
| Calibrate Quiet Room | button | On press | Calibrates noise floor from quiet room (3 seconds) |
| Calibrate Music Level | button | On press | Calibrates signal scaling from music playback (5 seconds) |
| Status LED | light | — | On-device RGB LED (ATOM Echo) |

## Configuration

    audio_reactive:
      microphone: mic_id          # Required: I2S microphone component ID
      update_interval: 50ms       # Processing interval (default: 50ms)
      beat_sensitivity: 50        # 1-100, higher = reacts to quieter onsets (default: 50)
      squelch: 10                 # 0-100, noise gate threshold (default: 10)

## Calibration

For best results, calibrate the device to your environment. Calibration data persists across reboots.

### Quiet room calibration (recommended)

Ensures the device correctly identifies silence and doesn't react to ambient noise.

1. Make sure the room is quiet (no music, minimal background noise)
2. **Double-click** the device button, or press **Calibrate Quiet Room** in Home Assistant
3. The LED glows green for 3 seconds while sampling
4. The LED brightens briefly to confirm calibration is complete

This sets the noise gate threshold and per-band noise floors based on your room's actual ambient noise level.

### Music level calibration (recommended)

Teaches the device what typical music levels look like in your setup, so the sensors produce a useful 0-1 range.

1. Play music at your typical listening volume
2. Press **Calibrate Music Level** in Home Assistant
3. The LED glows blue for 5 seconds while sampling
4. The LED brightens briefly to confirm calibration is complete

This sets the signal scaling factor so that typical music maps to mid-range sensor values (~0.5), giving the AGC room to normalize both quiet and loud passages.

### Calibration order

Run quiet room calibration first, then music calibration. If you change rooms, speaker setup, or device placement, re-run both calibrations.

## Button Actions (ATOM Echo)

| Action | What it does |
|--------|-------------|
| **Double click** | Calibrate quiet room (3-second green LED) |
| **Long press (1s+)** | Toggle microphone mute (red LED when muted) |

AGC reset and music calibration are available from Home Assistant only (button entities).

## Detection Modes

### Spectral flux (default)

Detects any sudden change in the frequency spectrum — kick drums, snare hits, cymbal crashes, piano attacks, violin pizzicato, vocal entrances. Works with all music genres including classical, jazz, and ambient.

### Bass energy

Only detects bass energy threshold crossings. Optimized for rhythmic music with a prominent low-frequency beat (EDM, pop, rock, hip-hop). Includes hysteresis to prevent rapid re-triggering.

Switch between modes via the Detection Mode select entity, or the integration sets it automatically per scene.

## How It Works

1. I2S microphone captures audio at 22,050 Hz sample rate
2. Ring buffer feeds samples to a dedicated FreeRTOS FFT task on core 0
3. 512-sample FFT with 75% overlap (~5.8ms per frame) produces frequency magnitudes
4. 16 frequency bands are computed with pink noise correction
5. PI-controller AGC normalizes energy values with configurable attack/release
6. Spectral flux onset detection identifies musical events across all frequency bands
7. Dynamics limiter and asymmetric EMA smoothing prevent jittery sensor output
8. Silence detector gates output when mid+high energy is below the calibrated threshold

## License

MIT
