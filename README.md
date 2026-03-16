# ESP32CAM-RTSP (Enhanced Fork)

This repository is based on:

- Upstream: https://github.com/rzeldent/esp32cam-rtsp

This fork keeps the original RTSP/MJPEG camera server foundation and adds runtime modules for OTA, diagnostics, telemetry, power management, and sleep automation.

## What this fork does

Core camera features:

- RTSP camera stream (`rtsp://<ip>:554/mjpeg/1`)
- HTTP MJPEG stream (`/stream`)
- HTTP JPEG snapshot (`/snapshot`)
- Web configuration UI (IotWebConf)

Fork-specific runtime features:

- OTA runtime with explicit password/port config and progress endpoint (`/ota/progress`)
- Telegram integration:
  - text notifications
  - snapshot upload
  - optional power telemetry appended to messages
- INA226 power telemetry runtime:
  - periodic sampling task
  - voltage/current/power reporting
  - mAh/mWh counters (RTC persisted)
- Diagnostics runtime:
  - RTSP stats (`/rtsp/stats`)
  - system stats (`/sys/stats`) with CPU load + RAM/PSRAM
- Power runtime profiles (`/power/profile`):
  - `eco` / `normal` / custom CPU MHz (80, 160, 240)
  - camera power + Wi-Fi PS tuning
- Wi-Fi power-save runtime (`/wifi/sleep`)
- Auto-sleep orchestration using:
  - RCWL motion sensor
  - light detector + dark-frame heuristic from JPEG size
  - grace windows before deep sleep
- Sleep control endpoints:
  - timer-based sleep (`/sleep?sec=N`)
  - deep sleep path (`/sleep?deep=1`)

## Runtime module layout

`main.cpp` remains the orchestrator, while reusable pieces are split into:

- `src/app/ota/ota.{h,cpp}`
- `src/app/telegram/telegram.{h,cpp}`
- `src/app/ina226/ina226.{h,cpp}`
- `src/app/power/power.{h,cpp}`
- `src/app/wifi/wifi.{h,cpp}`
- `src/app/diagnostics/diagnostics.{h,cpp}`

## Configuration and secrets

The project now expects secrets at:

- `src/config/secrets.h` (tracked)
- `src/config/secrets.local.h` (ignored by git)

Create `src/config/secrets.local.h` with at least:

```cpp
#define WIFI_SSID "your-ssid"
#define WIFI_PASSWORD "your-password"
#define AP_ADMIN_PASSWORD "your-admin-password"
#define OTA_PASSWORD "your-ota-password"
// optional:
// #define OTA_PORT 3232
```

`platformio.local.ini` is used for local OTA upload values and is ignored by git.

Example:

```ini
[ota]
host = 192.168.1.50
port = 3232
auth = your-ota-password
```

## Build and upload

Requirements:

- VS Code + PlatformIO, or PlatformIO Core CLI

Build:

```bash
platformio run
```

Upload over serial (example env):

```bash
platformio run -e esp32cam_seeed_xiao_esp32s3_sense_usb -t upload
```

Upload over OTA (default env in this repo):

```bash
platformio run -e esp32cam_seeed_xiao_esp32s3_sense_ota -t upload
```

Monitor:

```bash
platformio device monitor
```

## Supported board environments

Defined in `platformio.ini` / `boards/`:

- `esp32cam_ai_thinker`
- `esp32cam_espressif_esp_eye`
- `esp32cam_espressif_esp32s2_cam_board`
- `esp32cam_espressif_esp32s2_cam_header`
- `esp32cam_espressif_esp32s3_cam_lcd`
- `esp32cam_espressif_esp32s3_eye`
- `esp32cam_freenove_wrover_kit`
- `esp32cam_m5stack_camera_psram`
- `esp32cam_m5stack_camera`
- `esp32cam_m5stack_esp32cam`
- `esp32cam_m5stack_unitcam`
- `esp32cam_m5stack_unitcams3`
- `esp32cam_m5stack_wide`
- `esp32cam_m5stack_m5poecam_w`
- `esp32cam_seeed_xiao_esp32s3_sense_usb`
- `esp32cam_seeed_xiao_esp32s3_sense_ota`
- `esp32cam_ttgo_t_camera`
- `esp32cam_ttgo_t_journal`
- `m5stack-timer-cam`

## HTTP API (current firmware)

| Endpoint | Purpose | Notes |
|---|---|---|
| `/` | Main status/config page | HTML UI |
| `/config` | IotWebConf config page | UI handler |
| `/snapshot` | Single JPEG snapshot | Can trigger Telegram photo upload in flow |
| `/stream` | MJPEG stream | HTTP multipart stream |
| `/camera/config` | Camera + stream JSON status | Includes init result and image controls |
| `/rtsp/stats` | RTSP metrics | fps + sessions |
| `/sys/stats` | System diagnostics | CPU load, RAM, PSRAM |
| `/http/fps` | HTTP capture FPS control/stats | `?set=<float>` |
| `/power/profile` | Power profile control | `?mode=eco|normal[&mhz=80|160|240]`, or `?mhz=...` |
| `/wifi/sleep` | Wi-Fi PS mode status | JSON mode/active |
| `/ota/progress` | OTA progress percent | JSON `{pct}` |
| `/ina226` | INA226 telemetry | Voltage/current/power + counters |
| `/ina226/resetcounters` | Reset INA226 counters | JSON reset response |
| `/light/status` | Light input + dark-frame state | JSON |
| `/rcwl/status` | RCWL motion input state | JSON |
| `/autosleep` | Auto-sleep toggle/status | `?enable=0|1` |
| `/frame/dark` | Dark-frame heuristic debug | JPEG-size based |
| `/telegram` | Send Telegram message | `?msg=...` |
| `/sleep` | Sleep control | `?sec=<n>` or `?deep=1` |
| `/restart` | Reboot board | Immediate restart |
| `/flash` | Flash LED control | only if `FLASH_LED_GPIO` defined (`?v=<0..255>`) |

## Notes

- OTA auth in PlatformIO upload config should match the firmware OTA password.
- `src/config/secrets.local.h` and `platformio.local.ini` are intentionally git-ignored.
- Current default environment in `platformio.ini` is `esp32cam_seeed_xiao_esp32s3_sense_ota`.

## Credits

- Original project and architecture: https://github.com/rzeldent/esp32cam-rtsp
- Libraries used include IotWebConf, Micro-RTSP, and Arduino ESP32 ecosystem components.
