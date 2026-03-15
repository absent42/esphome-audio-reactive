# Contributing

Thanks for your interest in contributing to the ESPHome Audio Reactive component! This guide covers how to add support for new ESP32 devices.

## Adding a New Device

The audio-reactive component is hardware-agnostic — all device-specific configuration lives in YAML files. Adding a new device requires **no C++ code changes**, only a new YAML config and documentation updates.

### What you need

Before starting, gather these details about your device:

- **ESP32 variant** — ESP32, ESP32-S3, etc.
- **Microphone** — PDM or I2S codec? Which pins? If codec, which chip and I2C address?
- **Status feedback** — LED (type, count, GPIO, color order) or speaker (amp enable GPIO)?
- **Button(s)** — GPIO pin(s) and whether they use inverted logic
- **PSRAM** — does the device have PSRAM? (ESP32-S3 variants usually do)

### Step 1: Create the device YAML

Create `<device-name>.yaml` in the repo root. Use `atom-echo.yaml` as the reference for PDM microphone devices, or `waveshare-s3-audio.yaml` for I2S codec devices.

Your YAML must be **fully self-contained** — no shared includes or packages. It should compile and flash without any other files except the `components/` directory.

**Required sections:**

| Section | Purpose |
|---|---|
| `esphome:` | Device name and friendly name |
| `esp32:` | Board and framework (always `esp-idf`) |
| `psram:` | Required for ESP32-S3 with PSRAM |
| `wifi:` | Empty block (credentials via improv_serial) |
| `improv_serial:` | WiFi provisioning after flashing |
| `api:` | Home Assistant API |
| `logger:` | Logging |
| `external_components:` | `source: components` |
| `i2s_audio:` | I2S pin configuration |
| `microphone:` | Microphone platform config |
| `audio_reactive:` | Component config with triggers |
| `sensor:` | Bass/mid/high/amplitude/BPM |
| `binary_sensor:` | Onset + silence + device button |
| `number:` | Sensitivity + squelch |
| `select:` | Detection mode |
| `switch:` | Microphone mute |
| `button:` | Reset AGC + calibrate quiet + calibrate music |
| `light:` or `speaker:`/`rtttl:` | Feedback hardware |

**Button actions** (standard for all devices):

| Action | Function |
|---|---|
| Double-click | Calibrate quiet room |
| Triple-click | Calibrate music level |
| Long press (1s+) | Toggle microphone mute |

**Feedback colors** (standard for LED devices):

| Event | Color |
|---|---|
| Muted | Red |
| Quiet calibration | Green (pulsing during, solid on complete) |
| Music calibration | Blue (pulsing during, solid on complete) |

**PDM microphone devices** (like ATOM Echo):

```yaml
microphone:
  - platform: i2s_audio
    id: mic
    adc_type: external
    i2s_din_pin: GPIOxx    # PDM data pin
    pdm: true
    sample_rate: 22050
    bits_per_sample: 16bit
    channel: right

audio_reactive:
  sample_rate: 22050       # Must match microphone
```

**I2S codec devices** (like Waveshare):

```yaml
i2c:
  sda: GPIOxx
  scl: GPIOxx

audio_adc:
  - platform: es7210       # Or es8311, etc.
    address: 0x40

microphone:
  - platform: i2s_audio
    id: mic
    adc_type: external
    i2s_din_pin: GPIOxx
    pdm: false
    sample_rate: 44100
    bits_per_sample: 16bit

audio_reactive:
  sample_rate: 44100       # Must match microphone
```

### Step 2: Test locally

**Before submitting a PR, you must verify:**

1. Config validates: `esphome config <device>.yaml`
2. Firmware compiles: `esphome compile <device>.yaml`
3. Device boots without crashes
4. WiFi connects via improv_serial
5. Sensors appear in Home Assistant
6. Button actions work (double-click, triple-click, long-press)
7. Calibration works (quiet room + music level)
8. Feedback works (LED colors or speaker tones)

### Step 3: Add web installer support

Create `static/manifest-<device-name>.json`:

```json
{
    "name": "Audio Reactive Sensor (<Device Name>)",
    "version": "0.2.0",
    "builds": [
        {
            "chipFamily": "ESP32",
            "parts": [
                {
                    "path": "firmware/<device-name>.factory.bin",
                    "offset": 0
                }
            ]
        }
    ]
}
```

Use `"ESP32-S3"` for S3 variants.

Add a device card to `static/index.html`:

```html
<div class="device-card">
    <h2>Device Name</h2>
    <img src="images/<device-name>.jpg" alt="Device Name" />
    <p>Brief description.</p>
    <esp-web-install-button manifest="manifest-<device-name>.json">
        <button slot="activate">Install on Device Name</button>
    </esp-web-install-button>
</div>
```

Add a device image (`static/images/<device-name>.jpg`).

### Step 4: Update build matrix

Add your device to `.github/workflows/build.yml`:

```yaml
strategy:
  matrix:
    device:
      - name: atom-echo
        yaml: atom-echo.yaml
      # Add your device:
      - name: <device-name>
        yaml: <device-name>.yaml
```

### Step 5: Update documentation

- **README.md** — add to the hardware table and button actions table
- **Integration docs** — if you have access, add to `docs/audio-reactive-setup.md` in the integration repo

### ESP32-S3 notes

- Add `psram: mode: octal` and `speed: 80MHz` (with `ignore_not_found: false` if PSRAM is guaranteed)
- Add `wifi: power_save_mode: none` for reliable real-time streaming
- Use `board: esp32-s3-devkitc-1` unless a specific board exists
- Web installer users may need to hold BOOT while plugging USB — note this in the device card
- Manifest `chipFamily` must be `"ESP32-S3"`

## Reporting Issues

If you have a device that doesn't work correctly:

1. Enable debug logging: `debug_logging: true` in the `audio_reactive:` config
2. Share the full ESPHome device logs
3. Include your device model and the YAML you're using
4. Open an issue on GitHub

## Code Style

- YAML: 2-space indentation, no trailing spaces
- Keep configs self-contained (no includes/packages)
- Follow the existing naming conventions for entities
