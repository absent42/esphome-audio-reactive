# ESPHome Audio Reactive

ESP32 audio analysis component for ESPHome. Provides real-time onset/beat detection,
frequency band energy, amplitude, BPM estimation, and silence detection via on-device
FFT processing with a dedicated FreeRTOS task.

Designed as the audio source for
[Aqara Advanced Lighting](https://github.com/absent42/aqara-advanced-lighting)
audio-reactive dynamic scenes, but usable with any Home Assistant automation.

## Hardware

Can be adapted for any ESP32 with an I2S digital microphone. Configurations currently available for:

### M5Stack ATOM Echo

The best starting point for most users, readily available.

![M5Stack ATOM Echo](https://raw.githubusercontent.com/absent42/esphome-audio-reactive/refs/heads/main/static/images/atom-echo.jpg)

| | |
|---|---|
| **Price** | ~$13 |
| **Chipset** | ESP32-PICO-D4 |
| **Microphone** | Built-in SPM1423 PDM |
| **Power** | USB-C |
| **Size** | 24 x 24 x 17mm |
| **Where to buy** | [M5Stack store](https://shop.m5stack.com/products/atom-echo-smart-speaker-dev-kit), [Pi Hut](https://thepihut.com/products/atom-echo-smart-speaker-dev-kit), [Amazon US](https://amzn.to/4dnA7GH), [Amazon UK](https://amzn.to/4bdYJQM) |

### M5Stack ATOM Echo S3R

Higher-quality audio with an ES8311 codec and speaker feedback for on-device status tones.

![M5Stack ATOM Echo S3R](https://raw.githubusercontent.com/absent42/esphome-audio-reactive/refs/heads/main/static/images/atom-echo-s3r.jpg)

| | |
|---|---|
| **Price** | ~$15 |
| **Chipset** | ESP32-S3 |
| **Microphone** | MEMS via ES8311 ADC (I2S, 44.1kHz) |
| **Audio codec** | ES8311 (mic ADC + speaker DAC) |
| **Feedback** | Speaker tones (no LED) |
| **Power** | USB-C |
| **Where to buy** | [M5Stack store](https://shop.m5stack.com/products/atom-echo-s3r), [Pi Hut](https://thepihut.com/products/atom-echos3r-smart-speaker-dev-kit) |

### Waveshare ESP32-S3 Audio Board

Feature-rich board with dual MEMS microphones, 7-LED ring, and optional battery power.

![Waveshare ESP32-S3](https://raw.githubusercontent.com/absent42/esphome-audio-reactive/refs/heads/main/static/images/waveshare-esp32-s3.jpg)

| | |
|---|---|
| **Price** | ~$16 |
| **Chipset** | ESP32-S3R8 (8MB PSRAM) |
| **Microphone** | Dual MEMS via ES7210 ADC (I2S, 44.1kHz) |
| **Audio codec** | ES7210 (mic) + ES8311 (speaker) |
| **Feedback** | 7x WS2812 LED ring |
| **Power** | USB-C, optional battery |
| **Where to buy** | [Waveshare store](https://www.waveshare.com/esp32-s3-audio-board.htm), [Amazon US](https://amzn.to/4lqAoKU), [Amazon UK](https://amzn.to/414SkRU) |

### M5StickC Plus2

A compact development kit with a built-in screen, battery, and PDM microphone.

![M5StickC Plus2](https://raw.githubusercontent.com/absent42/esphome-audio-reactive/refs/heads/main/static/images/m5stack-stick2.jpg)

| | |
|---|---|
| **Price** | ~$20 |
| **Chipset** | ESP32-PICO-V3-02 |
| **Microphone** | Built-in SPM1423 PDM |
| **Power** | USB-C, built-in battery |
| **Size** | 54 x 25 x 16mm |
| **Where to buy** | [M5Stack store](https://shop.m5stack.com/products/m5stickc-plus2-esp32-mini-iot-development-kit), [Pi Hut](https://thepihut.com/products/m5stickc-plus2-esp32-mini-iot-development-kit), [Amazon US](https://amzn.to/470ydYE), [Amazon UK](https://amzn.to/4brZ0i4) |

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

See `atom-echo.yaml`, `atom-echo-s3r.yaml`, and `waveshare-s3-audio.yaml` for complete device-specific configs.

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
      sample_rate: 22050          # Sample rate in Hz, must match microphone config (default: 22050)
      fft_size: 512               # FFT window size: 256, 512, or 1024 (default: 512)
      debug_logging: false        # Enable comprehensive DSP pipeline logging (default: false)

## Automation Triggers

The component fires ESPHome automation triggers for status events, allowing device YAMLs to wire feedback to LEDs, speakers, or other hardware:

    audio_reactive:
      on_mute_changed:        # Fired when mute state changes (button or HA switch)
        - ...
      on_calibration_started: # Fired when quiet or music calibration begins
        - ...
      on_calibration_complete: # Fired when calibration finishes
        - ...
      on_silence_changed:     # Fired when silence state transitions
        - ...

See the device YAML files for examples of LED and speaker tone feedback.

## Debug Logging

Enable comprehensive DSP pipeline logging for troubleshooting:

    audio_reactive:
      debug_logging: true

When enabled, logs every 2 seconds: raw FFT magnitudes, scaled values, AGC gains, silence state, calibration state, published sensor values, sample rate, FFT size, and ring buffer fill level. Disable for production use.

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
2. **Triple-click** the device button, or press **Calibrate Music Level** in Home Assistant
3. The LED glows blue for 5 seconds while sampling
4. The LED brightens briefly to confirm calibration is complete

This sets the signal scaling factor so that typical music maps to mid-range sensor values (~0.5), giving the AGC room to normalize both quiet and loud passages.

### Calibration order

Run quiet room calibration first, then music calibration. If you change rooms, speaker setup, or device placement, re-run both calibrations.

## Button Actions

### ATOM Echo / ATOM Echo S3R / M5StickC Plus2

These devices have a single button — actions are distinguished by click pattern:

| Action | ATOM Echo | ATOM Echo S3R | M5StickC Plus2 |
|--------|-----------|---------------|----------------|
| **Double click** | Calibrate quiet (green LED) | Calibrate quiet (speaker tone) | Calibrate quiet (red LED) |
| **Triple click** | Calibrate music (blue LED) | Calibrate music (speaker tone) | Calibrate music (red LED) |
| **Long press (1s+)** | Toggle mute (red LED) | Toggle mute (speaker tone) | Toggle mute (red LED) |

### Waveshare ESP32-S3 Audio Board

The Waveshare has three dedicated buttons (K1, K2, K3) — one action per button:

| Button | Action | Feedback |
|--------|--------|----------|
| **K1** | Calibrate quiet room | Green LEDs |
| **K2** | Calibrate music level | Blue LEDs |
| **K3** | Toggle mute | Red LEDs |

## Detection Modes

### Spectral flux (default)

Detects any sudden change in the frequency spectrum — kick drums, snare hits, cymbal crashes, piano attacks, violin pizzicato, vocal entrances. Works with all music genres including classical, jazz, and ambient.

### Bass energy

Only detects bass energy threshold crossings. Optimized for rhythmic music with a prominent low-frequency beat (EDM, pop, rock, hip-hop). Includes hysteresis to prevent rapid re-triggering.

Switch between modes via the Detection Mode select entity, or the integration sets it automatically per scene.

## How It Works

1. Audio is captured at the configured sample rate (default 22,050 Hz for ATOM Echo, 44,100 Hz for S3R and Waveshare devices)
2. Ring buffer feeds samples to a dedicated FreeRTOS FFT task on core 0
3. Configurable FFT window (256, 512, or 1024 samples; default 512) with 75% overlap produces frequency magnitudes
4. 16 frequency bands are computed with pink noise correction
5. PI-controller AGC normalizes energy values with configurable attack/release
6. Spectral flux onset detection identifies musical events across all frequency bands
7. Dynamics limiter and asymmetric EMA smoothing prevent jittery sensor output
8. Silence detector gates output when mid+high energy is below the calibrated threshold

## License

MIT
