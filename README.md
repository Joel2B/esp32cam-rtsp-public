# ESP32CAM-RTSP (Enhanced Fork)

This repository is based on:

- Upstream: https://github.com/rzeldent/esp32cam-rtsp

This fork keeps the original RTSP/MJPEG camera server foundation and adds runtime modules for OTA, diagnostics, telemetry, power management, and sleep automation.

> Personal project note  
> This repository is maintained as a personal project and is intended to be used only with this hardware stack:
> - ESP32-S3
> - OV5640 camera
> - INA226 power monitor
> - RCWL-0516 motion sensor

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

## Web viewer

This firmware has a dedicated Next.js viewer app in a separate public repository:

- https://github.com/Joel2B/esp32cam-rtsp-viewer-public

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
#define TELEGRAM_MESSAGE_BOT_TOKEN "your-telegram-message-bot-token"
#define TELEGRAM_MESSAGE_CHAT_ID "your-telegram-message-chat-id"
// optional:
// #define OTA_PORT 3232
// #define TELEGRAM_PHOTO_BOT_TOKEN "your-telegram-photo-bot-token"
// #define TELEGRAM_PHOTO_CHAT_ID "your-telegram-photo-chat-id"
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

## Target hardware

This public project is scoped to:

- ESP32-S3
- OV5640 camera
- INA226 power monitor
- RCWL-0516 motion sensor

Default runtime pin mapping used by the firmware:

- `LIGHT_DO_GPIO = GPIO_NUM_1`
- `RCWL_DO_GPIO = GPIO_NUM_2`
- `I2C_WIRE_SDA` / `I2C_WIRE_SCL` from board definition (with fallback `5/6` in code)

## HTTP API (current firmware)

| Endpoint | Purpose | Notes |
|---|---|---|
| `/` | Main status/config page | HTML UI |
| `/config` | IotWebConf config page | UI handler |
| `/snapshot` | Single JPEG snapshot | Optional `?send=1` (Telegram upload), `?refresh=<sec>` |
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

## Useful endpoint examples

```bash
# Snapshot
curl "http://<ip>/snapshot"

# Snapshot + Telegram upload
curl "http://<ip>/snapshot?send=1"

# Stream
open "http://<ip>/stream"

# Limit HTTP producer FPS to 10
curl "http://<ip>/http/fps?set=10"

# Sleep by timer (seconds)
curl "http://<ip>/sleep?sec=60"

# Enter deep sleep (wake by RCWL ext1 path)
curl "http://<ip>/sleep?deep=1"

# Power profile
curl "http://<ip>/power/profile?mode=eco"
curl "http://<ip>/power/profile?mode=normal"
curl "http://<ip>/power/profile?mhz=160"
```

## Notes

- OTA auth in PlatformIO upload config should match the firmware OTA password.
- `src/config/secrets.local.h` and `platformio.local.ini` are intentionally git-ignored.
- Current default environment in `platformio.ini` is `esp32cam_seeed_xiao_esp32s3_sense_ota`.
- `/sleep?deep=1` uses RCWL wake path (EXT1). `/sleep?sec=<n>` uses timer wake.

## Security

- This project is intended for trusted/private networks.
- Control endpoints such as `/restart`, `/sleep`, `/power/profile`, and `/flash` can affect device availability and behavior.
- Never commit `src/config/secrets.local.h` or `platformio.local.ini` to git.

## Credits

- Original project and architecture: https://github.com/rzeldent/esp32cam-rtsp
- Libraries used include IotWebConf, Micro-RTSP, and Arduino ESP32 ecosystem components.
