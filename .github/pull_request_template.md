## Description

<!-- Brief description of what this PR does -->

## New Device Support

<!-- If adding a new device, fill out this section. Otherwise, delete it. -->

### Device Information

| | |
|---|---|
| **Device name** | |
| **ESP32 variant** | ESP32 / ESP32-S3 / other |
| **Microphone** | model, PDM or I2S codec |
| **Feedback** | LED (count, type) / Speaker / None |
| **Button(s)** | GPIO pin(s) |
| **Price** | approximate |
| **Where to buy** | link(s) |

### Checklist

- [ ] Created `<device-name>.yaml` (self-contained, no includes)
- [ ] `esphome config <device>.yaml` passes
- [ ] `esphome compile <device>.yaml` succeeds
- [ ] Tested on real hardware — device boots, WiFi connects
- [ ] Sensors appear in Home Assistant and publish data
- [ ] Button actions work (double-click, triple-click, long-press)
- [ ] Calibration works (quiet room + music level)
- [ ] Feedback works (LED colors or speaker tones)
- [ ] Created `static/manifest-<device-name>.json`
- [ ] Added device card to `static/index.html`
- [ ] Added device image to `static/images/`
- [ ] Added device to `.github/workflows/build.yml` matrix
- [ ] Updated `README.md` hardware table

### Test Evidence

<!-- Paste relevant log output showing the device working -->

```
[paste ESPHome device logs here]
```

## Other Changes

<!-- If this PR is not a new device, describe what it changes and why -->

### Testing

<!-- How was this tested? -->
