#include <Arduino.h>
#include <ESPmDNS.h>
#include <IotWebConf.h>
#include <IotWebConfTParameter.h>
#include <OV2640.h>
#include <WiFiClient.h>
#include <Wire.h>
#include <driver/i2c.h>
#include <driver/rtc_io.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_wifi.h>
#include <format_duration.h>
#include <format_number.h>
#include <lookup_camera_effect.h>
#include <lookup_camera_frame_size.h>
#include <lookup_camera_gainceiling.h>
#include <lookup_camera_wb_mode.h>
#include <moustache.h>
#include <rtsp_server.h>
#include <settings.h>
#include <soc/rtc_cntl_reg.h>

#include "app/diagnostics/diagnostics.h"
#include "app/ina226/ina226.h"
#include "app/ota/ota.h"
#include "app/power/power.h"
#include "app/telegram/telegram.h"
#include "app/wifi/wifi.h"
#include "config/secrets.h"
#include "esp32-hal-cpu.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define FRAME_ALIGN_KB 8
static constexpr uint64_t uS_PER_SEC = 1000000ULL;

#ifndef I2C_WIRE_SDA
#define I2C_WIRE_SDA 5
#endif
#ifndef I2C_WIRE_SCL
#define I2C_WIRE_SCL 6
#endif

// ===== Global runtime state (grouped by context) =====
// Hardware / sensor levels.
static bool power_camera = true;
static constexpr gpio_num_t LIGHT_DO_GPIO = GPIO_NUM_1;  // LM393 DO
static constexpr int DARK_LEVEL = HIGH;                  // 0=LOW (common on LM393), 1=HIGH
static constexpr int MOVEMENT_LEVEL = HIGH;              // 0=LOW (common on LM393), 1=HIGH
static constexpr gpio_num_t RCWL_DO_GPIO = GPIO_NUM_2;

// Auto-sleep / grace windows.
static constexpr uint32_t GRACE_MS = 5000;
static constexpr uint32_t GRACE1_MS = 5000;
static constexpr uint32_t GRACE2_MS = 30000;

enum class GraceReason : uint8_t {
    NONE,
    INIT,
    CANCEL,
};

static bool autoSleepEnabled = true;
static bool graceActive = false;

static GraceReason graceReason = GraceReason::NONE;
static uint32_t graceStartMs = 0;
static bool grace1Active = false;

static GraceReason grace1Status = GraceReason::NONE;
static uint32_t grace1Ms = 0;
static bool grace2Active = false;

static GraceReason grace2Status = GraceReason::NONE;
static uint32_t grace2Ms = 0;
static bool g_eco = false;

// Network / boot state.
static bool wifiWasConnected = false;
static const uint32_t BOOT_GRACE_MS = 2000;
static uint64_t g_boot_time_us = 0;
const char* TZ_INFO = "MST7";

static inline int read_light_do() {
    pinMode((int)LIGHT_DO_GPIO, INPUT);
    return digitalRead((int)LIGHT_DO_GPIO);
}

static inline int read_rcwl_do() {
    pinMode((int)RCWL_DO_GPIO, INPUT);
    return digitalRead((int)RCWL_DO_GPIO);
}

static bool telegram_power_provider(telegram_runtime::PowerTelemetry* out) {
    if (out == nullptr) return false;

    float vbus_V = 0.0f;
    float current_mA = 0.0f;
    bool ok = ina226_runtime::latest_power(&vbus_V, &current_mA);

    out->vbus_V = vbus_V;
    out->current_mA = current_mA;

    return ok;
}

static inline size_t round_up_block(size_t n) {
    const size_t B = (size_t)FRAME_ALIGN_KB * 1024;
    return (n + (B - 1)) & ~(B - 1);
}

// HTML files
extern const char index_html_min_start[] asm("_binary_html_index_min_html_start");

auto param_group_camera = iotwebconf::ParameterGroup("camera", "Camera settings");
auto param_frame_duration = iotwebconf::Builder<iotwebconf::UIntTParameter<unsigned long>>("fd").label("Frame duration (ms)").defaultValue(DEFAULT_FRAME_DURATION).min(0).build();
auto param_frame_size = iotwebconf::Builder<iotwebconf::SelectTParameter<sizeof(frame_sizes[0])>>("fs").label("Frame size").optionValues((const char*)&frame_sizes).optionNames((const char*)&frame_sizes).optionCount(sizeof(frame_sizes) / sizeof(frame_sizes[0])).nameLength(sizeof(frame_sizes[0])).defaultValue(DEFAULT_FRAME_SIZE).build();
auto param_jpg_quality = iotwebconf::Builder<iotwebconf::UIntTParameter<byte>>("q").label("JPG quality").defaultValue(DEFAULT_JPEG_QUALITY).min(1).max(100).build();
auto param_brightness = iotwebconf::Builder<iotwebconf::IntTParameter<int>>("b").label("Brightness").defaultValue(DEFAULT_BRIGHTNESS).min(-2).max(2).build();
auto param_contrast = iotwebconf::Builder<iotwebconf::IntTParameter<int>>("c").label("Contrast").defaultValue(DEFAULT_CONTRAST).min(-2).max(2).build();
auto param_saturation = iotwebconf::Builder<iotwebconf::IntTParameter<int>>("s").label("Saturation").defaultValue(DEFAULT_SATURATION).min(-2).max(2).build();
auto param_special_effect = iotwebconf::Builder<iotwebconf::SelectTParameter<sizeof(camera_effects[0])>>("e").label("Effect").optionValues((const char*)&camera_effects).optionNames((const char*)&camera_effects).optionCount(sizeof(camera_effects) / sizeof(camera_effects[0])).nameLength(sizeof(camera_effects[0])).defaultValue(DEFAULT_EFFECT).build();
auto param_whitebal = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("wb").label("White balance").defaultValue(DEFAULT_WHITE_BALANCE).build();
auto param_awb_gain = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("awbg").label("Automatic white balance gain").defaultValue(DEFAULT_WHITE_BALANCE_GAIN).build();
auto param_wb_mode = iotwebconf::Builder<iotwebconf::SelectTParameter<sizeof(camera_wb_modes[0])>>("wbm").label("White balance mode").optionValues((const char*)&camera_wb_modes).optionNames((const char*)&camera_wb_modes).optionCount(sizeof(camera_wb_modes) / sizeof(camera_wb_modes[0])).nameLength(sizeof(camera_wb_modes[0])).defaultValue(DEFAULT_WHITE_BALANCE_MODE).build();
auto param_exposure_ctrl = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("ec").label("Exposure control").defaultValue(DEFAULT_EXPOSURE_CONTROL).build();
auto param_aec2 = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("aec2").label("Auto exposure (dsp)").defaultValue(DEFAULT_AEC2).build();
auto param_ae_level = iotwebconf::Builder<iotwebconf::IntTParameter<int>>("ael").label("Auto Exposure level").defaultValue(DEFAULT_AE_LEVEL).min(-2).max(2).build();
auto param_aec_value = iotwebconf::Builder<iotwebconf::IntTParameter<int>>("aecv").label("Manual exposure value").defaultValue(DEFAULT_AEC_VALUE).min(9).max(1200).build();
auto param_gain_ctrl = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("gc").label("Gain control").defaultValue(DEFAULT_GAIN_CONTROL).build();
auto param_agc_gain = iotwebconf::Builder<iotwebconf::IntTParameter<int>>("agcg").label("AGC gain").defaultValue(DEFAULT_AGC_GAIN).min(0).max(30).build();
auto param_gain_ceiling = iotwebconf::Builder<iotwebconf::SelectTParameter<sizeof(camera_gain_ceilings[0])>>("gcl").label("Auto Gain ceiling").optionValues((const char*)&camera_gain_ceilings).optionNames((const char*)&camera_gain_ceilings).optionCount(sizeof(camera_gain_ceilings) / sizeof(camera_gain_ceilings[0])).nameLength(sizeof(camera_gain_ceilings[0])).defaultValue(DEFAULT_GAIN_CEILING).build();
auto param_bpc = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("bpc").label("Black pixel correct").defaultValue(DEFAULT_BPC).build();
auto param_wpc = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("wpc").label("White pixel correct").defaultValue(DEFAULT_WPC).build();
auto param_raw_gma = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("rg").label("Gamma correct").defaultValue(DEFAULT_RAW_GAMMA).build();
auto param_lenc = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("lenc").label("Lens correction").defaultValue(DEFAULT_LENC).build();
auto param_hmirror = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("hm").label("Horizontal mirror").defaultValue(DEFAULT_HORIZONTAL_MIRROR).build();
auto param_vflip = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("vm").label("Vertical mirror").defaultValue(DEFAULT_VERTICAL_MIRROR).build();
auto param_dcw = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("dcw").label("Downsize enable").defaultValue(DEFAULT_DCW).build();
auto param_colorbar = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("cb").label("Colorbar").defaultValue(DEFAULT_COLORBAR).build();

OV2640 cam;
DNSServer dnsServer;
std::unique_ptr<rtsp_server> camera_server = nullptr;
WebServer web_server(80);

auto thingName = String(WIFI_SSID) + "-" + String(ESP.getEfuseMac(), 16);
IotWebConf iotWebConf(thingName.c_str(), &dnsServer, &web_server, WIFI_PASSWORD, CONFIG_VERSION);

esp_err_t camera_init_result;

// Stream HTTP/RTSP: buffers, semaphores, and task state.
#ifndef MAX_HTTP_STREAMS
#define MAX_HTTP_STREAMS 3
#endif

static SemaphoreHandle_t g_http_stream_sem = nullptr;

// Shared JPEG buffer (producer/consumer).
static SemaphoreHandle_t g_cam_mutex = nullptr;    // protect camera access (init/deinit + run/getfb)
static SemaphoreHandle_t g_frame_mutex = nullptr;  // protect shared frame buffer
static uint8_t* g_frame = nullptr;
static size_t g_frame_len = 0;

// Stream pacing/control telemetry.
static float g_http_fps_target = 0.0f;  // 0 = unlimited
static SemaphoreHandle_t g_http_fps_mutex = nullptr;
static volatile uint32_t g_prod_frame_count = 0;
static volatile uint64_t g_prod_last_ts_us = 0;
static float g_prod_fps_ema = 0.0f;
static size_t g_frame_cap = 0;
static volatile uint32_t g_frame_seq = 0;

static TaskHandle_t g_capture_task = nullptr;
static volatile bool g_capture_should_run = false;
static int32_t g_http_clients = 0;  // active HTTP stream clients
static bool g_capture_starting = false;
static SemaphoreHandle_t g_capture_exit_sem = nullptr;
static TaskHandle_t s_rtspTask = nullptr;
static bool g_stream_sync_ready = false;
static portMUX_TYPE g_sb_mux = portMUX_INITIALIZER_UNLOCKED;

// Post-WiFi: events, tasks, and sensor state.
static bool g_post_wifi_runtime_inited = false;

// Light sensor (frame-darkness detection).
static bool light_status = true;
static const size_t DARK_SIZE_THRESHOLD = 17000;
static const uint32_t LIGHT_CHECK_INTERVAL_MS = 1000;
static const uint8_t DARK_CONFIRM_REQUIRED = 2;
static bool g_light_present = true;  // assume light is present at startup
static SemaphoreHandle_t g_light_mutex = nullptr;
static TaskHandle_t s_lightSensorTask = nullptr;
static TaskHandle_t s_lightFrameTask = nullptr;

// RCWL task handle.
static TaskHandle_t s_rcwlSensorTask = nullptr;

static inline int32_t http_clients_load() {
    return __atomic_load_n(&g_http_clients, __ATOMIC_RELAXED);
}

static inline int32_t http_clients_inc() {
    return __atomic_add_fetch(&g_http_clients, 1, __ATOMIC_ACQ_REL);
}

static inline int32_t http_clients_dec() {
    return __atomic_sub_fetch(&g_http_clients, 1, __ATOMIC_ACQ_REL);
}

static inline TaskHandle_t capture_task_load() {
    return __atomic_load_n(&g_capture_task, __ATOMIC_ACQUIRE);
}

static inline void capture_task_store(TaskHandle_t h) {
    __atomic_store_n(&g_capture_task, h, __ATOMIC_RELEASE);
}

static bool diag_rtsp_running() {
    return camera_server != nullptr;
}

static float diag_rtsp_fps() {
    return (camera_server != nullptr) ? camera_server->get_fps() : 0.0f;
}

static size_t diag_rtsp_sessions() {
    return (camera_server != nullptr) ? camera_server->num_connected() : 0;
}

static bool power_camera_is_on() {
    return camera_init_result == ESP_OK;
}

static int power_get_cpu_mhz() {
    return (int)getCpuFrequencyMhz();
}

static void power_set_cpu_mhz(int mhz) {
    setCpuFrequencyMhz((uint32_t)mhz);
}

void handle_root() {
    log_v("Handle root");
    // Let IotWebConf test and handle captive portal requests.
    if (iotWebConf.handleCaptivePortal())
        return;

    // Format hostname
    auto hostname = "esp32-" + WiFi.macAddress() + ".local";
    hostname.replace(":", "");
    hostname.toLowerCase();

    // Wifi Modes
    const char* wifi_modes[] = {"NULL", "STA", "AP", "STA+AP"};
    auto ipv4 = WiFi.getMode() == WIFI_MODE_AP ? WiFi.softAPIP() : WiFi.localIP();
    auto ipv6 = WiFi.getMode() == WIFI_MODE_AP ? WiFi.softAPIPv6() : WiFi.localIPv6();

    auto initResult = esp_err_to_name(camera_init_result);

    if (initResult == nullptr)
        initResult = "Unknown reason";

    moustache_variable_t substitutions[] = {
        // Version / CPU
        {"AppTitle", APP_TITLE},
        {"AppVersion", APP_VERSION},
        {"BoardType", BOARD_NAME},
        {"ThingName", iotWebConf.getThingName()},
        {"SDKVersion", ESP.getSdkVersion()},
        {"ChipModel", ESP.getChipModel()},
        {"ChipRevision", String(ESP.getChipRevision())},
        {"CpuFreqMHz", String(ESP.getCpuFreqMHz())},
        {"CpuCores", String(ESP.getChipCores())},
        {"FlashSize", format_memory(ESP.getFlashChipSize(), 0)},
        {"HeapSize", format_memory(ESP.getHeapSize())},
        {"PsRamSize", format_memory(ESP.getPsramSize(), 0)},
        // Diagnostics
        {"Uptime", String(format_duration(millis() / 1000))},
        {"FreeHeap", format_memory(ESP.getFreeHeap())},
        {"MaxAllocHeap", format_memory(ESP.getMaxAllocHeap())},
        {"NumRTSPSessions", camera_server != nullptr ? String(camera_server->num_connected()) : "RTSP server disabled"},
        // Network
        {"HostName", hostname},
        {"MacAddress", WiFi.macAddress()},
        {"AccessPoint", WiFi.SSID()},
        {"SignalStrength", String(WiFi.RSSI())},
        {"WifiMode", wifi_modes[WiFi.getMode()]},
        {"IPv4", ipv4.toString()},
        {"IPv6", ipv6.toString()},
        {"NetworkState.ApMode", String(iotWebConf.getState() == iotwebconf::NetworkState::ApMode)},
        {"NetworkState.OnLine", String(iotWebConf.getState() == iotwebconf::NetworkState::OnLine)},
        // Camera
        {"FrameSize", String(param_frame_size.value())},
        {"FrameDuration", String(param_frame_duration.value())},
        {"FrameFrequency", String(1000.0 / param_frame_duration.value(), 1)},
        {"JpegQuality", String(param_jpg_quality.value())},
        {"CameraInitialized", String(camera_init_result == ESP_OK)},
        {"CameraInitResult", String(camera_init_result)},
        {"CameraInitResultText", initResult},
        // Settings
        {"Brightness", String(param_brightness.value())},
        {"Contrast", String(param_contrast.value())},
        {"Saturation", String(param_saturation.value())},
        {"SpecialEffect", String(param_special_effect.value())},
        {"WhiteBal", String(param_whitebal.value())},
        {"AwbGain", String(param_awb_gain.value())},
        {"WbMode", String(param_wb_mode.value())},
        {"ExposureCtrl", String(param_exposure_ctrl.value())},
        {"Aec2", String(param_aec2.value())},
        {"AeLevel", String(param_ae_level.value())},
        {"AecValue", String(param_aec_value.value())},
        {"GainCtrl", String(param_gain_ctrl.value())},
        {"AgcGain", String(param_agc_gain.value())},
        {"GainCeiling", String(param_gain_ceiling.value())},
        {"Bpc", String(param_bpc.value())},
        {"Wpc", String(param_wpc.value())},
        {"RawGma", String(param_raw_gma.value())},
        {"Lenc", String(param_lenc.value())},
        {"HMirror", String(param_hmirror.value())},
        {"VFlip", String(param_vflip.value())},
        {"Dcw", String(param_dcw.value())},
        {"ColorBar", String(param_colorbar.value())},
        // RTSP
        {"RtspPort", String(RTSP_PORT)},
        {"RtspFps", camera_server != nullptr ? String(camera_server->get_fps(), 1) : "0.0"}};

    web_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    auto html = moustache_render(index_html_min_start, substitutions);
    web_server.send(200, "text/html", html);
}

#ifdef FLASH_LED_GPIO
void handle_flash() {
    log_v("handle_flash");
    // If no value present, use off, otherwise convert v to integer. Depends on analog resolution for max value
    auto v = web_server.hasArg("v") ? web_server.arg("v").toInt() : 0;
    // If conversion fails, v = 0
    analogWrite(FLASH_LED_GPIO, v);

    web_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    web_server.send(200);
}
#endif

struct SharedBuf {
    uint8_t* data;
    size_t len;
    volatile int ref;  // reference count (2 = HTTP and TG)
};

static inline SharedBuf* sb_acquire(uint8_t* data, size_t len) {
    auto* sb = (SharedBuf*)heap_caps_malloc(sizeof(SharedBuf),
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!sb) return nullptr;
    sb->data = data;  // buffer in SPIRAM (ok)
    sb->len = len;
    sb->ref = 1;

    return sb;
}

// Increment refcount (call when handing buffer to another consumer)
static inline void sb_addref(SharedBuf* sb) {
    portENTER_CRITICAL(&g_sb_mux);
    sb->ref++;
    portEXIT_CRITICAL(&g_sb_mux);
}

// Release one ref; when it reaches 0, free(buffer) and free(struct)
static inline void sb_release(SharedBuf* sb) {
    bool doFree = false;
    portENTER_CRITICAL(&g_sb_mux);
    int r = --sb->ref;
    if (r == 0) doFree = true;
    portEXIT_CRITICAL(&g_sb_mux);

    if (doFree) {
        if (sb->data) free(sb->data);
        free(sb);
    }
}

struct TgUploadArgs {
    SharedBuf* sb;  // shared
    char filename[48];
    char caption[96];
};

void TgUploadTask(void* pv) {
    auto* args = static_cast<TgUploadArgs*>(pv);  // use pv
    bool ok = telegram_runtime::sendPhotoBuffer(args->sb->data, args->sb->len,
                                                args->filename, args->caption);
    Serial.println(ok ? "[TG] upload OK" : "[TG] upload FAIL");

    sb_release(args->sb);  // release its ref
    free(args);            // free struct created in handle_snapshot
    vTaskDelete(nullptr);
}

void handle_snapshot() {
    if (!g_stream_sync_ready || g_frame_mutex == nullptr || g_cam_mutex == nullptr) {
        web_server.send(503, "text/plain", "stream_sync_not_ready");
        return;
    }

    if (camera_init_result != ESP_OK) {
        web_server.send(503, "text/plain", "Camera is not initialized");
        return;
    }

    // Copy latest frame (single copy)
    uint8_t* buf = nullptr;
    size_t flen = 0;

    if (xSemaphoreTake(g_frame_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (g_frame && g_frame_len) {
            flen = g_frame_len;
            buf = (uint8_t*)heap_caps_malloc(flen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (buf) memcpy(buf, g_frame, flen);
        }
        xSemaphoreGive(g_frame_mutex);
    }

    if (!buf) {
        if (g_cam_mutex) xSemaphoreTake(g_cam_mutex, portMAX_DELAY);

        cam.run();  // single run only
        const uint8_t* fb = (const uint8_t*)cam.getfb();
        size_t fb_len = cam.getSize();

        if (g_cam_mutex) xSemaphoreGive(g_cam_mutex);

        if (!fb || fb_len == 0) {
            web_server.send(500, "text/plain", "Unable to obtain frame buffer from the camera");
            return;
        }

        flen = fb_len;
        buf = (uint8_t*)heap_caps_malloc(flen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (!buf) {
            web_server.send(500, "text/plain", "No memory for snapshot copy");
            return;
        }

        memcpy(buf, fb, flen);
    }

    if (!buf || flen == 0) {
        if (buf) free(buf);

        web_server.send(503, "text/plain", "No frame available");
        return;
    }

    // Create SharedBuf with ref=1
    SharedBuf* sb = sb_acquire(buf, flen);

    if (!sb) {
        free(buf);
        web_server.send(500, "text/plain", "No memory for sb");
        return;
    }

    // If Telegram send is requested, launch task and add ref
    if (web_server.hasArg("send")) {
        TgUploadArgs* args = (TgUploadArgs*)heap_caps_malloc(sizeof(TgUploadArgs), MALLOC_CAP_8BIT);

        if (args) {
            args->sb = sb;
            strncpy(args->filename, "frame.jpg", sizeof(args->filename) - 1);
            args->filename[sizeof(args->filename) - 1] = '\0';
            strncpy(args->caption, "ESP32-S3 snapshot", sizeof(args->caption) - 1);
            args->caption[sizeof(args->caption) - 1] = '\0';

            sb_addref(sb);  // now there are 2 owners: HTTP and TG

            if (xTaskCreatePinnedToCore(TgUploadTask, "TgUploadTask",
                                        20480, args, 1, nullptr, 1) != pdPASS) {
                // on failure, undo addref and free args
                sb_release(sb);  // revert extra ref
                free(args);
                Serial.println("[TG] could not create TgUploadTask");
            }
        } else {
            Serial.println("[TG] no memory for args");
        }
    }

    if (sb->len == 0) {
        // something went wrong; avoid setContentLength(0)
        free(sb->data);  // sb_acquire took ownership of the buffer
        free(sb);
        web_server.send(503, "text/plain", "Empty frame");
        return;
    }

    Serial.printf("[SNAP] flen=%u\n", sb->len);

    // Respond over HTTP using the same buffer
    web_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");

    if (web_server.hasArg("refresh")) {
        int refreshSec = web_server.arg("refresh").toInt();

        if (refreshSec < 1) refreshSec = 1;
        if (refreshSec > 3600) refreshSec = 3600;

        web_server.sendHeader("Refresh", String(refreshSec));
    }

    web_server.send_P(200, "image/jpeg", (const char*)sb->data, sb->len);

    // HTTP is done with the buffer: release its ref
    sb_release(sb);
}

#define STREAM_CONTENT_BOUNDARY "123456789000000000000987654321"

// --- Producer helpers ---
static bool ensure_frame_capacity(size_t need) {
    if (need <= g_frame_cap) return true;
    // size_t new_cap = (need + 16383) & ~((size_t)16383);
    size_t new_cap = round_up_block(need);

    uint8_t* n = (uint8_t*)heap_caps_malloc(new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!n) return false;

    // Keep allocator work out of the hot lock path: only pointer swap is locked.
    if (g_frame_mutex == nullptr) {
        uint8_t* old = g_frame;
        g_frame = n;
        g_frame_cap = new_cap;
        if (old) free(old);

        return true;
    }

    uint8_t* old = nullptr;
    if (xSemaphoreTake(g_frame_mutex, portMAX_DELAY) != pdTRUE) {
        free(n);
        return false;
    }

    // Re-check under lock in case capacity changed while allocating.
    if (need <= g_frame_cap) {
        xSemaphoreGive(g_frame_mutex);
        free(n);
        return true;
    }

    old = g_frame;
    g_frame = n;
    g_frame_cap = new_cap;
    xSemaphoreGive(g_frame_mutex);

    if (old) free(old);

    return true;
}

static void capture_task(void* pv) {
    uint64_t t_prev = esp_timer_get_time();
    uint64_t win_start = t_prev;
    uint32_t win_frames = 0;

    while (g_capture_should_run && camera_init_result == ESP_OK) {
        // lock camera while reading sensor frame
        if (g_cam_mutex) xSemaphoreTake(g_cam_mutex, portMAX_DELAY);

        cam.run();
        const uint8_t* fb = (const uint8_t*)cam.getfb();
        size_t flen = cam.getSize();

        if (g_cam_mutex) xSemaphoreGive(g_cam_mutex);

        bool produced = false;

        if (fb && flen > 0) {
            // Ensure capacity OUTSIDE g_frame_mutex
            // to avoid malloc/free while streamer waits for frame
            bool cap_ok = ensure_frame_capacity(flen);

            if (cap_ok) {
                // now lock only to copy and update metadata
                if (xSemaphoreTake(g_frame_mutex, portMAX_DELAY) == pdTRUE) {
                    memcpy(g_frame, fb, flen);
                    g_frame_len = flen;
                    g_frame_seq++;
                    produced = true;
                    xSemaphoreGive(g_frame_mutex);
                }
            }
        }

        // Telemetry and pacing only if a frame was actually produced
        if (produced) {
            const uint64_t now = esp_timer_get_time();
            const uint64_t dt = now - t_prev;
            t_prev = now;

            // optional pacing by target FPS
            float target;

            if (g_http_fps_mutex) xSemaphoreTake(g_http_fps_mutex, portMAX_DELAY);
            target = g_http_fps_target;
            if (g_http_fps_mutex) xSemaphoreGive(g_http_fps_mutex);

            if (target > 0.0f) {
                const float frame_us = 1000000.0f / target;

                if ((float)dt < frame_us) {
                    const uint32_t wait_ms =
                        (uint32_t)((frame_us - (float)dt) / 1000.0f);

                    if (wait_ms > 0) {
                        vTaskDelay(pdMS_TO_TICKS(wait_ms));
                    }
                }
            }

            // FPS accounting (window ~1s)
            win_frames++;

            if (now - win_start >= 1000000) {
                const float fps_window =
                    (float)win_frames * 1000000.0f / (float)(now - win_start);
                win_start = now;

                if (g_http_fps_mutex) xSemaphoreTake(g_http_fps_mutex, portMAX_DELAY);

                g_prod_frame_count += win_frames;
                g_prod_last_ts_us = now;
                g_prod_fps_ema = (g_prod_fps_ema == 0.0f)
                                     ? fps_window
                                     : (0.6f * g_prod_fps_ema + 0.4f * fps_window);

                if (g_http_fps_mutex) xSemaphoreGive(g_http_fps_mutex);

                win_frames = 0;
            }
        }

        // fallback when no target: honor frame_duration (>0)
        if (g_http_fps_target <= 0.0f) {
            uint32_t ms = param_frame_duration.value();

            if (ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(ms));
            }
        }
    }

    g_capture_should_run = false;
    capture_task_store(nullptr);

    if (g_capture_exit_sem) {
        xSemaphoreGive(g_capture_exit_sem);
    }

    vTaskDelete(nullptr);
}

static bool start_capture_task_if_needed() {
    if (camera_init_result != ESP_OK) return false;

    if (__atomic_exchange_n(&g_capture_starting, true, __ATOMIC_ACQ_REL)) {
        // Another caller is starting capture_task; wait briefly for visibility.
        for (int i = 0; i < 10; ++i) {
            if (capture_task_load() != nullptr) return true;
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        return capture_task_load() != nullptr;
    }

    bool ok = true;
    TaskHandle_t current = capture_task_load();

    if (!current) {
        if (g_capture_exit_sem) {
            (void)xSemaphoreTake(g_capture_exit_sem, 0);  // drain stale exit signal from previous runs
        }

        g_capture_should_run = true;
        TaskHandle_t created = nullptr;

        BaseType_t rc = xTaskCreatePinnedToCore(
            capture_task, "capture", 8192, nullptr, 4, &created, 1);

        if (rc == pdPASS && created != nullptr) {
            capture_task_store(created);
        } else {
            g_capture_should_run = false;
            ok = false;
        }
    }

    __atomic_store_n(&g_capture_starting, false, __ATOMIC_RELEASE);
    return ok;
}

static void stop_capture_task() {
    TaskHandle_t h = capture_task_load();

    if (!h) return;

    g_capture_should_run = false;

    if (g_capture_exit_sem) {
        (void)xSemaphoreTake(g_capture_exit_sem, pdMS_TO_TICKS(500));
    } else {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void handle_http_fps() {
    // Change FPS if ?set=... is provided
    if (web_server.hasArg("set")) {
        float v = web_server.arg("set").toFloat();

        if (v < 0.0f) v = 0.0f;      // 0 = unlimited
        if (v > 120.0f) v = 120.0f;  // reasonable cap
        if (g_http_fps_mutex) xSemaphoreTake(g_http_fps_mutex, portMAX_DELAY);
        g_http_fps_target = v;
        if (g_http_fps_mutex) xSemaphoreGive(g_http_fps_mutex);
    }

    // Snapshot producer metrics
    uint32_t frames;
    uint64_t last_us;
    float ema;
    float target;

    if (g_http_fps_mutex) xSemaphoreTake(g_http_fps_mutex, portMAX_DELAY);
    frames = g_prod_frame_count;
    last_us = g_prod_last_ts_us;
    ema = g_prod_fps_ema;
    target = g_http_fps_target;
    if (g_http_fps_mutex) xSemaphoreGive(g_http_fps_mutex);

    uint64_t now = esp_timer_get_time();
    uint64_t since_us = (last_us > 0 && now > last_us) ? (now - last_us) : 0;

    String json = "{";
    json += "\"target\":" + String(target, 2);
    json += ",\"fps_avg\":" + String(ema, 2);     // compat
    json += ",\"sensor_fps\":" + String(ema, 2);  // clearer alias
    json += ",\"frames\":" + String(frames);
    json += ",\"last_frame_ms\":" + String((double)since_us / 1000.0, 2);
    json += "}";

    web_server.send(200, "application/json", json);
}

static inline void tiny_pause() {
    // If only one client, yield instead of sleeping 1ms for better FPS
    if (http_clients_load() == 1) {
        taskYIELD();
    } else {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void http_stream_task(void* pv) {
    WiFiClient* client = reinterpret_cast<WiFiClient*>(pv);

    if (!client) {
        xSemaphoreGive(g_http_stream_sem);
        vTaskDelete(nullptr);
        return;
    }

    // Wi-Fi/CPU turbo
    setCpuFrequencyMhz(240);
    vTaskDelay(pdMS_TO_TICKS(100));
    wifi_runtime::set_ps("none");

    client->setNoDelay(true);
    client->setTimeout(1000);

    http_clients_inc();

    client->write(
        "HTTP/1.1 200 OK\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Keep-Alive: timeout=10, max=1000\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=" STREAM_CONTENT_BOUNDARY "\r\n\r\n");

    char size_buf[128];
    uint32_t last_seq = 0;

    size_t tx_cap = 0;
    uint8_t* tx = nullptr;

    while (client->connected()) {
        if (camera_init_result != ESP_OK || !g_capture_should_run || g_eco)
            break;

        // 1) quick snapshot
        uint32_t seq_snapshot = 0;
        size_t flen_snapshot = 0;

        if (xSemaphoreTake(g_frame_mutex, pdMS_TO_TICKS(1)) == pdTRUE) {
            seq_snapshot = g_frame_seq;
            flen_snapshot = g_frame_len;
            xSemaphoreGive(g_frame_mutex);
        } else {
            tiny_pause();
            continue;
        }

        // new frame available?
        if (seq_snapshot == last_seq || flen_snapshot == 0) {
            tiny_pause();
            continue;
        }

        // 2) ensure client buffer outside mutex
        if (flen_snapshot > tx_cap) {
            uint8_t* n = (uint8_t*)heap_caps_malloc(
                flen_snapshot,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (n) {
                if (tx) free(tx);
                tx = n;
                tx_cap = flen_snapshot;
            }
        }

        if (!tx) {
            tiny_pause();
            continue;
        }

        // 3) copy latest frame into local buffer
        size_t flen_final = 0;

        if (xSemaphoreTake(g_frame_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            if (g_frame && g_frame_len > 0) {
                flen_final = g_frame_len;

                if (flen_final <= tx_cap) {
                    memcpy(tx, g_frame, flen_final);
                    last_seq = g_frame_seq;
                } else {
                    flen_final = 0;
                }
            }
            xSemaphoreGive(g_frame_mutex);
        }

        if (flen_final == 0) {
            taskYIELD();
            continue;
        }

        // 4) send multipart header
        int hdrlen = snprintf(
            size_buf, sizeof(size_buf),
            "--" STREAM_CONTENT_BOUNDARY
            "\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %u\r\n\r\n",
            (unsigned)flen_final);

        int wrote_hdr = client->write((const uint8_t*)size_buf, hdrlen);
        if (wrote_hdr <= 0) break;

        // 5) send frame in MTU-ish chunks
        size_t off = 0;
        while (off < flen_final) {
            size_t chunk = flen_final - off;
            if (chunk > 1460) chunk = 1460;

            size_t sent = client->write(tx + off, chunk);
            if (sent == 0) break;

            off += sent;
            taskYIELD();  // yield so capture keeps running
        }

        if (off < flen_final) {
            break;
        }

        // no sleep here: next frame may be ready immediately
    }

    if (tx) free(tx);

    client->stop();
    delete client;

    xSemaphoreGive(g_http_stream_sem);

    if (http_clients_dec() <= 0) {
        __atomic_store_n(&g_http_clients, 0, __ATOMIC_RELEASE);
        stop_capture_task();
        wifi_runtime::set_ps("max");  // save power when nobody is watching
    }

    vTaskDelete(nullptr);
}

void handle_stream() {
    log_v("handle_stream");

    if (!g_stream_sync_ready || g_http_stream_sem == nullptr || g_frame_mutex == nullptr || g_cam_mutex == nullptr) {
        web_server.send(503, "text/plain", "stream_sync_not_ready");
        return;
    }

    if (camera_init_result != ESP_OK || g_eco || !power_camera) {
        web_server.send(500, "text/plain", "Camera is not initialized");
        return;
    }

    // limit concurrency
    if (xSemaphoreTake(g_http_stream_sem, 0) != pdTRUE) {
        web_server.send(503, "text/plain", "Too many /stream clients. Try later.");
        return;
    }

    // start single producer if needed
    if (!start_capture_task_if_needed()) {
        xSemaphoreGive(g_http_stream_sem);
        web_server.send(503, "text/plain", "Failed to start capture task");
        return;
    }

    // spawn client task and return immediately
    WiFiClient* cptr = new WiFiClient(web_server.client());

    if (!cptr) {
        xSemaphoreGive(g_http_stream_sem);
        web_server.send(500, "text/plain", "No memory for stream client");
        return;
    }

    // USE BOTH CORES: no affinity pinning for client task
    BaseType_t ok = xTaskCreatePinnedToCore(http_stream_task, "http_stream", 8192, cptr, 5, nullptr, 0);

    if (ok != pdPASS) {
        delete cptr;
        xSemaphoreGive(g_http_stream_sem);
        web_server.send(500, "text/plain", "Failed to start stream task");
        return;
    }
}

void start_rtsp_server() {
    if (camera_init_result != ESP_OK || camera_server)
        return;

    log_v("start_rtsp_server");
    camera_server = std::unique_ptr<rtsp_server>(new rtsp_server(cam, param_frame_duration.value(), RTSP_PORT));
    camera_server->setNoDelay(true);

    MDNS.addService("rtsp", "udp", RTSP_PORT);
}

static void power_on_camera() {
    if (power_camera)
        return;

    sensor_t* sensor = esp_camera_sensor_get();

    if (sensor) {
        log_i("0x02");
        sensor->set_reg(sensor, 0x3008, 0xFF, 0x02);
        power_camera = true;
    }

    start_rtsp_server();
}

static void power_off_camera() {
    if (!power_camera)
        return;

    stop_capture_task();

    sensor_t* sensor = esp_camera_sensor_get();

    if (sensor) {
        log_i("1 << 6");
        sensor->set_reg(sensor, 0x3008, 0xFF, 1 << 6);
        // sensor->set_reg(sensor, 0x3008, 0x40, 0x00);
        power_camera = false;
    }
}

esp_err_t initialize_camera() {
    log_v("initialize_camera");

    log_i("Frame size: %s", param_frame_size.value());
    auto frame_size = lookup_frame_size(param_frame_size.value());
    log_i("JPEG quality: %d", param_jpg_quality.value());
    auto jpeg_quality = param_jpg_quality.value();
    log_i("Frame duration: %d ms", param_frame_duration.value());

    const camera_config_t camera_config = {
        .pin_pwdn = CAMERA_CONFIG_PIN_PWDN,          // GPIO pin for camera power down line
        .pin_reset = CAMERA_CONFIG_PIN_RESET,        // GPIO pin for camera reset line
        .pin_xclk = CAMERA_CONFIG_PIN_XCLK,          // GPIO pin for camera XCLK line
        .pin_sccb_sda = CAMERA_CONFIG_PIN_SCCB_SDA,  // GPIO pin for camera SDA line
        .pin_sccb_scl = CAMERA_CONFIG_PIN_SCCB_SCL,  // GPIO pin for camera SCL line
        .pin_d7 = CAMERA_CONFIG_PIN_Y9,              // GPIO pin for camera D7 line
        .pin_d6 = CAMERA_CONFIG_PIN_Y8,              // GPIO pin for camera D6 line
        .pin_d5 = CAMERA_CONFIG_PIN_Y7,              // GPIO pin for camera D5 line
        .pin_d4 = CAMERA_CONFIG_PIN_Y6,              // GPIO pin for camera D4 line
        .pin_d3 = CAMERA_CONFIG_PIN_Y5,              // GPIO pin for camera D3 line
        .pin_d2 = CAMERA_CONFIG_PIN_Y4,              // GPIO pin for camera D2 line
        .pin_d1 = CAMERA_CONFIG_PIN_Y3,              // GPIO pin for camera D1 line
        .pin_d0 = CAMERA_CONFIG_PIN_Y2,              // GPIO pin for camera D0 line
        .pin_vsync = CAMERA_CONFIG_PIN_VSYNC,        // GPIO pin for camera VSYNC line
        .pin_href = CAMERA_CONFIG_PIN_HREF,          // GPIO pin for camera HREF line
        .pin_pclk = CAMERA_CONFIG_PIN_PCLK,          // GPIO pin for camera PCLK line
        .xclk_freq_hz = CAMERA_CONFIG_CLK_FREQ_HZ,   // Frequency of XCLK signal, in Hz. EXPERIMENTAL: Set to 16MHz on ESP32-S2 or ESP32-S3 to enable EDMA mode
        .ledc_timer = CAMERA_CONFIG_LEDC_TIMER,      // LEDC timer to be used for generating XCLK
        .ledc_channel = CAMERA_CONFIG_LEDC_CHANNEL,  // LEDC channel to be used for generating XCLK
        .pixel_format = PIXFORMAT_JPEG,              // Format of the pixel data: PIXFORMAT_ + YUV422|GRAYSCALE|RGB565|JPEG
        .frame_size = frame_size,                    // Size of the output image: FRAMESIZE_ + QVGA|CIF|VGA|SVGA|XGA|SXGA|UXGA
        .jpeg_quality = jpeg_quality,                // Quality of JPEG output. 0-63 lower means higher quality
        .fb_count = CAMERA_CONFIG_FB_COUNT,          // Number of frame buffers to be allocated. If more than one, then each frame will be acquired (double speed)
        .fb_location = CAMERA_CONFIG_FB_LOCATION,    // The location where the frame buffer will be allocated
        .grab_mode = CAMERA_GRAB_LATEST,             // When buffers should be filled
#if CONFIG_CAMERA_CONVERTER_ENABLED
        conv_mode = CONV_DISABLE,  // RGB<->YUV Conversion mode
#endif
        .sccb_i2c_port = SCCB_I2C_PORT  // If pin_sccb_sda is -1, use the already configured I2C bus by number
    };

    return cam.init(camera_config);
}

static void stop_rtsp_and_camera() {
    if (camera_server) {
        camera_server.reset();
    }

    esp_camera_deinit();  // power down camera (sensor + DCMI)
    camera_init_result = ESP_FAIL;
}

void isolate(gpio_num_t gpio_num) {
    rtc_gpio_init(gpio_num);
    rtc_gpio_pullup_dis(gpio_num);
    rtc_gpio_pulldown_dis(gpio_num);
    rtc_gpio_set_direction(gpio_num, RTC_GPIO_MODE_DISABLED);
    rtc_gpio_isolate(gpio_num);
}

static void configure_sleep_low_power() {
    isolate(GPIO_NUM_1);
    isolate(GPIO_NUM_3);
    isolate(GPIO_NUM_4);
    isolate(GPIO_NUM_7);
    isolate(GPIO_NUM_8);
    isolate(GPIO_NUM_9);
    isolate(GPIO_NUM_41);
    isolate(GPIO_NUM_42);

    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_AUTO);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC8M, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, ESP_PD_OPTION_OFF);
}

static void prepare_for_sleep() {
    power_off_camera();
    vTaskDelay(pdMS_TO_TICKS(20));

    stop_rtsp_and_camera();
    vTaskDelay(pdMS_TO_TICKS(500));

    wifi_runtime::set_ps("max");
    vTaskDelay(pdMS_TO_TICKS(500));

    setCpuFrequencyMhz(80);
    vTaskDelay(pdMS_TO_TICKS(500));

    ina226_runtime::shutdown();
    vTaskDelay(pdMS_TO_TICKS(500));
}

void deep_sleep(String id) {
    log_i("deep_sleep");
    telegram_runtime::send("deep_sleep: " + id);

    prepare_for_sleep();

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext1_wakeup(1ULL << RCWL_DO_GPIO, ESP_EXT1_WAKEUP_ANY_HIGH);

    configure_sleep_low_power();

    esp_deep_sleep_start();
}

void deep_sleep_timer(uint64_t seconds) {
    Serial.printf("[SLEEP] Sleeping %llu s...\n", (unsigned long long)seconds);
    Serial.flush();

    telegram_runtime::send(String("deep_sleep_timer: ") + String((unsigned long long)seconds));

    prepare_for_sleep();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_timer_wakeup(seconds * uS_PER_SEC);
    configure_sleep_low_power();
    esp_deep_sleep_start();
}

void handle_sleep() {
    if (web_server.hasArg("deep")) {
        web_server.send(200, "text/plain", "");
        deep_sleep("deep_sleep");
        return;
    }

    // Validate that "sec" parameter is present
    if (!web_server.hasArg("sec")) {
        web_server.send(400, "text/plain", "Missing parameter 'sec'");
        return;
    }

    // Read parameter
    String secStr = web_server.arg("sec");
    uint64_t seconds = secStr.toInt();

    // Sanitize odd values
    if (seconds == 0) {
        seconds = 1;  // minimum 1 second to avoid glitches
    }

    // Respond to client BEFORE sleeping
    String msg = "OK, sleeping for " + String((unsigned long long)seconds) + " seconds...\n";
    web_server.send(200, "text/plain", msg);

    // Small delay so TCP response can leave over Wi-Fi
    delay(100);

    // Sleep
    deep_sleep_timer(seconds);
}

void handle_restart() {
    web_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    web_server.send(200, "text/plain", "Restarting board...\n");
    delay(1000);
    ESP.restart();
}

void sendMessageTelegram() {
    String msg = web_server.hasArg("msg") ? web_server.arg("msg") : "";
    String response = telegram_runtime::send(msg);

    web_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    web_server.send(200, "text/plain", response);
}

static inline void startGrace(GraceReason reason) {
    graceActive = true;
    graceReason = reason;
    graceStartMs = millis();
}

static inline void cancelGrace() {
    graceActive = false;
    graceReason = GraceReason::CANCEL;
}

static inline void startGrace1(GraceReason status) {
    grace1Active = true;
    grace1Status = status;
    grace1Ms = millis();
}

static inline void cancelGrace1() {
    grace1Active = false;
    grace1Status = GraceReason::CANCEL;
}

static inline void startGrace2(GraceReason status) {
    grace2Active = true;
    grace2Status = status;
    grace2Ms = millis();
}

static inline void cancelGrace2() {
    grace2Active = false;
    grace2Status = GraceReason::CANCEL;
}

enum : EventBits_t {
    WIFI_BIT_CONNECTED = (1 << 0),
    WIFI_BIT_DISCONNECTED = (1 << 1),
};

// Returns true if the latest frame appears dark/black
static bool is_frame_dark_guess() {
    uint64_t now_us = esp_timer_get_time();
    uint32_t sinceBoot_ms = (uint32_t)((now_us - g_boot_time_us) / 1000ULL);

    if (sinceBoot_ms < BOOT_GRACE_MS) {
        // Optional debug:
        // Serial.printf("[LIGHTCHK] grace %u/%u ms -> force LIGHT\n",
        //               sinceBoot_ms, BOOT_GRACE_MS);
        return false;  // force "not dark"
    }

    if (!g_stream_sync_ready || g_frame_mutex == nullptr) {
        return false;
    }

    if (!light_status)
        return true;

    if (camera_init_result != ESP_OK || !g_capture_should_run || g_eco) {
        // If camera is not ready, this method cannot estimate darkness.
        // Assume NOT dark to avoid false "no light" triggers.
        return false;
    }

    size_t jpegLen = 0;
    uint64_t last_us = 0;

    // read g_frame_len
    if (xSemaphoreTake(g_frame_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        jpegLen = g_frame_len;
        xSemaphoreGive(g_frame_mutex);
    }

    // read last frame timestamp
    if (g_http_fps_mutex) xSemaphoreTake(g_http_fps_mutex, portMAX_DELAY);
    last_us = g_prod_last_ts_us;
    if (g_http_fps_mutex) xSemaphoreGive(g_http_fps_mutex);

    // if no recent frame exists, avoid declaring darkness yet
    uint32_t age_ms = (uint32_t)((now_us - last_us) / 1000ULL);

    // if frame is old (e.g. >5s), do not trust it either
    if (age_ms > 5000) {
        // Camera is not producing fresh frames. Do not assume dark.
        return false;
    }

    // Main heuristic:
    // very small JPEG => near-black scene
    bool darkGuess = (jpegLen > 0 && jpegLen < DARK_SIZE_THRESHOLD);

    // Optional debug:
    // Serial.printf("[LIGHTCHK] jpegLen=%u age_ms=%u darkGuess=%d\n",
    //               (unsigned)jpegLen, (unsigned)age_ms, (int)darkGuess);

    return darkGuess;
}

static void set_light_state(bool present) {
    bool oldVal;

    if (g_light_mutex) xSemaphoreTake(g_light_mutex, portMAX_DELAY);
    oldVal = g_light_present;
    g_light_present = present;
    if (g_light_mutex) xSemaphoreGive(g_light_mutex);

    // If state changed, log or trigger existing logic
    if (present != oldVal) {
        if (present) {
            Serial.println("[LIGHT] Light is present now.");
            // call your normal "light detected" flow here
            // e.g. light_on_handler();
        } else {
            Serial.println("[LIGHT] No light now (dark).");
            // call your normal "no light" flow here
            // e.g. light_off_handler();
        }
    }
}

bool is_light_present_now() {
    bool val;

    if (g_light_mutex) xSemaphoreTake(g_light_mutex, portMAX_DELAY);
    val = g_light_present;
    if (g_light_mutex) xSemaphoreGive(g_light_mutex);

    return val;
}

void light_frame_task(void* pvParam) {
    Serial.println("[LIGHT] light_frame_task started.");

    uint8_t consecutive_dark = 0;

    for (;;) {
        // Wait for next measurement interval
        vTaskDelay(pdMS_TO_TICKS(LIGHT_CHECK_INTERVAL_MS));

        bool dark = is_frame_dark_guess();

        if (dark) {
            consecutive_dark++;
            Serial.printf("[LIGHT] dark frame detected (%u/%u)\n",
                          (unsigned)consecutive_dark,
                          (unsigned)DARK_CONFIRM_REQUIRED);
        } else {
            // if light is seen, reset counter
            if (consecutive_dark > 0) {
                Serial.println("[LIGHT] frame is not dark, reset counter");
            }

            consecutive_dark = 0;
            // mark "light present"
            set_light_state(true);
            continue;
        }

        // Have we reached the required consecutive dark readings?
        if (consecutive_dark >= DARK_CONFIRM_REQUIRED) {
            // confirm "no light"
            set_light_state(false);
            // keep counter capped to avoid overflow
            consecutive_dark = DARK_CONFIRM_REQUIRED;
        }
    }
}

static void light_sensor_task(void* arg) {
    const char* TAG = "[light_sensor_task] ";

    const TickType_t period = pdMS_TO_TICKS(3000);
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, 0);

        if (bits & WIFI_BIT_CONNECTED) {
            log_i("%s WIFI_BIT_CONNECTED", TAG);

            if (autoSleepEnabled && !graceActive) {
                telegram_runtime::send("init");
                startGrace(GraceReason::INIT);
            }
        }

        if (bits & WIFI_BIT_DISCONNECTED) {
            log_i("%s WIFI_BIT_DISCONNECTED", TAG);
            vTaskDelayUntil(&last, period);
            continue;
        }

        if (!graceActive || !autoSleepEnabled) {
            vTaskDelayUntil(&last, period);
            continue;
        }

        const bool value = !is_light_present_now();

        if (value && light_status) {
            power_off_camera();
            light_status = false;
            log_i("%s %s", TAG, "light off");
            // telegram_runtime::send("light off");
        }

        if (millis() - graceStartMs < GRACE_MS) {
            vTaskDelayUntil(&last, period);
            continue;
        }

        if (value && !grace1Active) {
            startGrace1(GraceReason::INIT);
        }

        if (!value) {
            cancelGrace1();
        }

        if (!grace1Active) {
            vTaskDelayUntil(&last, period);
            continue;
        }

        if (millis() - grace1Ms < GRACE1_MS) {
            vTaskDelayUntil(&last, period);
            continue;
        }

        deep_sleep(TAG);
    }
}

static void rcwl_sensor_task(void* arg) {
    const char* TAG = "[rcwl_sensor_task] ";

    const TickType_t period = pdMS_TO_TICKS(2000);
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, 0);

        if (bits & WIFI_BIT_CONNECTED) {
            log_i("%s WIFI_BIT_CONNECTED", TAG);

            if (autoSleepEnabled && !graceActive) {
                telegram_runtime::send("init");
                startGrace(GraceReason::INIT);
            }
        }

        if (bits & WIFI_BIT_DISCONNECTED) {
            log_i("%s WIFI_BIT_DISCONNECTED", TAG);
            vTaskDelayUntil(&last, period);
            continue;
        }

        if (!graceActive || !autoSleepEnabled) {
            vTaskDelayUntil(&last, period);
            continue;
        }

        const bool value = read_rcwl_do() == MOVEMENT_LEVEL;

        if (value) {
            log_i("%s %s", TAG, "rcwl");

            if (autoSleepEnabled) {
                telegram_runtime::send("rcwl");

                int mhz = getCpuFrequencyMhz();
                int max_mhz = 240;

                if (mhz != max_mhz) {
                    setCpuFrequencyMhz(max_mhz);
                    delay(20);
                    power_on_camera();
                    wifi_runtime::set_ps("none");
                    g_eco = false;
                    light_status = true;
                }
            }
        }

        if (millis() - graceStartMs < GRACE_MS) {
            vTaskDelayUntil(&last, period);
            continue;
        }

        if (!grace2Active || value) {
            startGrace2(GraceReason::INIT);
            cancelGrace1();
        }

        if (millis() - grace2Ms < GRACE2_MS) {
            vTaskDelayUntil(&last, period);
            continue;
        }

        deep_sleep(TAG);
    }
}

static void rtsp_task(void* arg) {
    const TickType_t period = pdMS_TO_TICKS(1000);
    TickType_t last = xTaskGetTickCount();

    String mode = "";

    for (;;) {
        if (!camera_server) {
            vTaskDelayUntil(&last, period);
            continue;
        }

        if (camera_server->num_connected() > 0) {
            const bool isDark = read_light_do() == DARK_LEVEL;
            String value = isDark ? "max" : "none";

            if (value != mode) {
                wifi_runtime::set_ps(value.c_str());
                mode = value;
            }

            vTaskDelayUntil(&last, period);
            continue;
        }

        if (mode != "") {
            wifi_runtime::set_ps("max");
            mode = "";
        }

        vTaskDelayUntil(&last, period);
    }
}

// Wi-Fi handler -> sets bits (Arduino callback: WiFi.onEvent)
static void wifi_event_handler(WiFiEvent_t event) {
    uint32_t bit = 0;

    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            log_i("AP_STACONNECTED");
            bit = WIFI_BIT_CONNECTED;
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            log_i("AP_STADISCONNECTED");
            bit = WIFI_BIT_DISCONNECTED;
            break;
        default:
            break;
    }

    if (s_lightSensorTask)
        xTaskNotify(s_lightSensorTask, bit, eSetBits);

    if (s_rcwlSensorTask)
        xTaskNotify(s_rcwlSensorTask, bit, eSetBits);
}

static bool init_post_wifi_runtime(String* error_out = nullptr) {
    if (g_post_wifi_runtime_inited) return true;

    String err = "";

    if (err.length() == 0 && g_light_mutex == nullptr) {
        g_light_mutex = xSemaphoreCreateMutex();
        if (g_light_mutex == nullptr) err = "xSemaphoreCreateMutex(g_light_mutex)";
    }

    if (err.length() == 0 && g_http_fps_mutex == nullptr) {
        g_http_fps_mutex = xSemaphoreCreateMutex();
        if (g_http_fps_mutex == nullptr) err = "xSemaphoreCreateMutex(g_http_fps_mutex)";
    }

    if (err.length() == 0 && g_http_stream_sem == nullptr) {
        g_http_stream_sem = xSemaphoreCreateCounting(MAX_HTTP_STREAMS, MAX_HTTP_STREAMS);
        if (g_http_stream_sem == nullptr) err = "xSemaphoreCreateCounting(g_http_stream_sem)";
    }

    if (err.length() == 0 && g_cam_mutex == nullptr) {
        g_cam_mutex = xSemaphoreCreateMutex();
        if (g_cam_mutex == nullptr) err = "xSemaphoreCreateMutex(g_cam_mutex)";
    }

    if (err.length() == 0 && g_frame_mutex == nullptr) {
        g_frame_mutex = xSemaphoreCreateMutex();
        if (g_frame_mutex == nullptr) err = "xSemaphoreCreateMutex(g_frame_mutex)";
    }

    if (err.length() == 0 && g_capture_exit_sem == nullptr) {
        g_capture_exit_sem = xSemaphoreCreateBinary();
        if (g_capture_exit_sem == nullptr) err = "xSemaphoreCreateBinary(g_capture_exit_sem)";
    }

    if (err.length() == 0 && !ina226_runtime::is_available()) {
        ina226_runtime::start_task(&err);
    }

    if (err.length() == 0 && s_lightSensorTask == nullptr) {
        BaseType_t rc = xTaskCreatePinnedToCore(
            light_sensor_task,
            "light_sensor_task",
            4096,
            nullptr,
            3,
            &s_lightSensorTask,
            1);

        if (rc != pdPASS || s_lightSensorTask == nullptr) {
            err = "xTaskCreatePinnedToCore(light_sensor_task)";
        }
    }

    if (err.length() == 0 && s_lightFrameTask == nullptr) {
        BaseType_t rc = xTaskCreatePinnedToCore(
            light_frame_task,
            "light_frame_task",
            4096,
            nullptr,
            3,
            &s_lightFrameTask,
            1);

        if (rc != pdPASS || s_lightFrameTask == nullptr) {
            err = "xTaskCreatePinnedToCore(light_frame_task)";
        }
    }

    if (err.length() == 0 && s_rcwlSensorTask == nullptr) {
        BaseType_t rc = xTaskCreatePinnedToCore(
            rcwl_sensor_task,
            "rcwl_sensor_task",
            4096,
            nullptr,
            3,
            &s_rcwlSensorTask,
            1);

        if (rc != pdPASS || s_rcwlSensorTask == nullptr) {
            err = "xTaskCreatePinnedToCore(rcwl_sensor_task)";
        }
    }

    if (err.length() == 0) {
        g_stream_sync_ready = true;
        WiFi.onEvent(wifi_event_handler);

        // Tasks were created after the first connect event: seed initial state once.
        if (s_lightSensorTask) xTaskNotify(s_lightSensorTask, WIFI_BIT_CONNECTED, eSetBits);
        if (s_rcwlSensorTask) xTaskNotify(s_rcwlSensorTask, WIFI_BIT_CONNECTED, eSetBits);

        g_post_wifi_runtime_inited = true;
        return true;
    }

    g_stream_sync_ready = false;
    ina226_runtime::set_available(false);

    if (error_out) *error_out = err;
    return false;
}

void handle_light_status() {
    int raw = read_light_do();
    // If module LED turns on when "dark", usually DO=LOW -> interpret as "triggered"
    bool dark = raw == DARK_LEVEL;
    bool dark_frame = !is_light_present_now();

    String json = "{";
    json += "\"ok\":true";
    json += ",\"gpio\":" + String((int)LIGHT_DO_GPIO);
    json += ",\"raw\":" + String(raw);
    json += ",\"active_level\":\"" + String(DARK_LEVEL == 0 ? "LOW" : "HIGH") + "\"";
    json += ",\"dark\":" + String(dark ? "true" : "false");
    json += ",\"dark_frame\":" + String(dark_frame ? "true" : "false");
    json += "}";

    web_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    web_server.sendHeader("Refresh", "1");
    web_server.send(200, "application/json", json);
}

void handle_rcwl_status() {
    int raw = read_rcwl_do();
    bool triggered = raw == MOVEMENT_LEVEL;

    String json = "{";
    json += "\"ok\":true";
    json += ",\"gpio\":" + String((int)RCWL_DO_GPIO);
    json += ",\"raw\":" + String(raw);
    json += ",\"active_level\":\"" + String(MOVEMENT_LEVEL == 0 ? "LOW" : "HIGH") + "\"";
    json += ",\"state\":\"" + String(triggered ? "triggered" : "idle") + "\"";
    json += "}";

    web_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    web_server.sendHeader("Refresh", "1");
    web_server.send(200, "application/json", json);
}

void handle_auto_sleep() {
    // /light/auto_sleep?enable=0|1
    if (web_server.hasArg("enable")) {
        String v = web_server.arg("enable");
        bool wantEnable = (v != "0");

        if (!wantEnable) {
            autoSleepEnabled = false;
            cancelGrace();
            cancelGrace1();
            cancelGrace2();
        } else {
            autoSleepEnabled = true;
            startGrace(GraceReason::INIT);
        }
    }

    uint32_t remaining = 0;
    uint32_t remaining2 = 0;
    uint32_t remaining3 = 0;

    if (graceActive) {
        uint32_t elapsed = millis() - graceStartMs;
        remaining = (elapsed >= GRACE_MS) ? 0 : (GRACE_MS - elapsed);
    }

    if (grace1Active) {
        uint32_t elapsed = millis() - grace1Ms;
        remaining2 = (elapsed >= GRACE1_MS) ? 0 : (GRACE1_MS - elapsed);
    }

    if (grace2Active) {
        uint32_t elapsed = millis() - grace2Ms;
        remaining3 = (elapsed >= GRACE2_MS) ? 0 : (GRACE2_MS - elapsed);
    }

    String json = "{";
    json += "\"ok\":true";
    json += ",\"enabled\":" + String(autoSleepEnabled ? "true" : "false");
    json += ",\"grace_active\":" + String(graceActive ? "true" : "false");
    json += ",\"grace1_active\":" + String(grace1Active ? "true" : "false");
    json += ",\"grace2_active\":" + String(grace2Active ? "true" : "false");
    json += ",\"grace_reason\":\"" + String(graceReason == GraceReason::INIT ? "INIT" : graceReason == GraceReason::CANCEL ? "CANCEL"
                                                                                                                           : "NONE") +
            "\"";
    json += ",\"grace_remaining_ms\":" + String(remaining);
    json += ",\"grace1_remaining_ms\":" + String(remaining2);
    json += ",\"grace2_remaining_ms\":" + String(remaining3);
    json += "}";

    web_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    web_server.send(200, "application/json", json);
}

void update_camera_settings() {
    auto camera = esp_camera_sensor_get();
    if (camera == nullptr) {
        log_e("Unable to get camera sensor");
        return;
    }

    camera->set_brightness(camera, param_brightness.value());
    camera->set_contrast(camera, param_contrast.value());
    camera->set_saturation(camera, param_saturation.value());
    camera->set_special_effect(camera, lookup_camera_effect(param_special_effect.value()));
    camera->set_whitebal(camera, param_whitebal.value());
    camera->set_awb_gain(camera, param_awb_gain.value());
    camera->set_wb_mode(camera, lookup_camera_wb_mode(param_wb_mode.value()));
    camera->set_exposure_ctrl(camera, param_exposure_ctrl.value());
    camera->set_aec2(camera, param_aec2.value());
    camera->set_ae_level(camera, param_ae_level.value());
    camera->set_aec_value(camera, param_aec_value.value());
    camera->set_gain_ctrl(camera, param_gain_ctrl.value());
    camera->set_agc_gain(camera, param_agc_gain.value());
    camera->set_gainceiling(camera, lookup_camera_gainceiling(param_gain_ceiling.value()));
    camera->set_bpc(camera, param_bpc.value());
    camera->set_wpc(camera, param_wpc.value());
    camera->set_raw_gma(camera, param_raw_gma.value());
    camera->set_lenc(camera, param_lenc.value());
    camera->set_hmirror(camera, param_hmirror.value());
    camera->set_vflip(camera, param_vflip.value());
    camera->set_dcw(camera, param_dcw.value());
    camera->set_colorbar(camera, param_colorbar.value());
}

void print_wakeup_reason() {
    esp_sleep_wakeup_cause_t wakeup_reason;

    wakeup_reason = esp_sleep_get_wakeup_cause();

    switch (wakeup_reason) {
        case ESP_SLEEP_WAKEUP_EXT0:
            Serial.println("Wakeup caused by external signal using RTC_IO");
            break;
        case ESP_SLEEP_WAKEUP_EXT1:
            Serial.println("Wakeup caused by external signal using RTC_CNTL");
            break;
        case ESP_SLEEP_WAKEUP_TIMER:
            Serial.println("Wakeup caused by timer");
            break;
        case ESP_SLEEP_WAKEUP_TOUCHPAD:
            Serial.println("Wakeup caused by touchpad");
            break;
        case ESP_SLEEP_WAKEUP_ULP:
            Serial.println("Wakeup caused by ULP program");
            break;
        case ESP_SLEEP_WAKEUP_GPIO:
            Serial.println("Wakeup caused by GPIO");
            break;
        default:
            Serial.printf("Wakeup was not caused by deep sleep: %d\n", wakeup_reason);
            break;
    }
}

void on_connected() {
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    esp_bt_mem_release(ESP_BT_MODE_BTDM);

    wifi_runtime::set_ps("max");
    print_wakeup_reason();
    start_rtsp_server();

    String post_wifi_err = "";

    if (!init_post_wifi_runtime(&post_wifi_err)) {
        String msg = "[BOOT] post-wifi init failed: " + post_wifi_err;
        log_e("%s", msg.c_str());
        telegram_runtime::send(msg);
    }

    configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
    g_boot_time_us = esp_timer_get_time();
}

void on_config_saved() {
    log_v("on_config_saved");
    update_camera_settings();
}

void handle_camera_config() {
    const char* initText = esp_err_to_name(camera_init_result);
    if (initText == nullptr) initText = "Unknown reason";

    String fb_location = (CAMERA_CONFIG_FB_LOCATION == CAMERA_FB_IN_PSRAM) ? "PSRAM" : "DRAM";
    float fps_target = 1000.0f / (float)param_frame_duration.value();
    float rtsp_fps = (camera_server != nullptr) ? camera_server->get_fps() : 0.0f;
    size_t rtsp_sessions = (camera_server != nullptr) ? camera_server->num_connected() : 0;

    String json = "{";

    json += "\"camera_initialized\":" + String(camera_init_result == ESP_OK ? "true" : "false") + ",";
    json += "\"camera_init_result\":" + String((int)camera_init_result) + ",";
    json += "\"camera_init_text\":\"" + String(initText) + "\",";

    json += "\"rtsp\":{";
    json += "\"port\":" + String(RTSP_PORT) + ",";
    json += "\"fps_real\":" + String(rtsp_fps, 2) + ",";
    json += "\"sessions\":" + String(rtsp_sessions);
    json += "},";

    json += "\"timing\":{";
    json += "\"frame_duration_ms\":" + String(param_frame_duration.value()) + ",";
    json += "\"fps_target\":" + String(fps_target, 2);
    json += "},";

    json += "\"frame\":{";
    json += "\"size\":\"" + String(param_frame_size.value()) + "\",";
    json += "\"jpeg_quality\":" + String(param_jpg_quality.value()) + ",";
    json += "\"fb_count\":" + String(CAMERA_CONFIG_FB_COUNT) + ",";
    json += "\"fb_location\":\"" + fb_location + "\"";
    json += "},";

    json += "\"image_controls\":{";
    json += "\"brightness\":" + String(param_brightness.value()) + ",";
    json += "\"contrast\":" + String(param_contrast.value()) + ",";
    json += "\"saturation\":" + String(param_saturation.value()) + ",";
    json += "\"effect\":\"" + String(param_special_effect.value()) + "\",";
    json += "\"white_balance\":" + String(param_whitebal.value() ? "true" : "false") + ",";
    json += "\"awb_gain\":" + String(param_awb_gain.value() ? "true" : "false") + ",";
    json += "\"wb_mode\":\"" + String(param_wb_mode.value()) + "\",";
    json += "\"exposure_ctrl\":" + String(param_exposure_ctrl.value() ? "true" : "false") + ",";
    json += "\"aec2\":" + String(param_aec2.value() ? "true" : "false") + ",";
    json += "\"ae_level\":" + String(param_ae_level.value()) + ",";
    json += "\"aec_value\":" + String(param_aec_value.value()) + ",";
    json += "\"gain_ctrl\":" + String(param_gain_ctrl.value() ? "true" : "false") + ",";
    json += "\"agc_gain\":" + String(param_agc_gain.value()) + ",";
    json += "\"gain_ceiling\":\"" + String(param_gain_ceiling.value()) + "\",";
    json += "\"bpc\":" + String(param_bpc.value() ? "true" : "false") + ",";
    json += "\"wpc\":" + String(param_wpc.value() ? "true" : "false") + ",";
    json += "\"raw_gma\":" + String(param_raw_gma.value() ? "true" : "false") + ",";
    json += "\"lenc\":" + String(param_lenc.value() ? "true" : "false") + ",";
    json += "\"hmirror\":" + String(param_hmirror.value() ? "true" : "false") + ",";
    json += "\"vflip\":" + String(param_vflip.value() ? "true" : "false") + ",";
    json += "\"dcw\":" + String(param_dcw.value() ? "true" : "false") + ",";
    json += "\"colorbar\":" + String(param_colorbar.value() ? "true" : "false");
    json += "}";

    json += "}";

    web_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    web_server.send(200, "application/json", json);
}

void handle_frame_dark() {
    if (!g_stream_sync_ready || g_frame_mutex == nullptr) {
        web_server.send(503, "application/json",
                        "{\"ok\":false,\"error\":\"stream_sync_not_ready\"}");
        return;
    }

    // if camera is not initialized, return logical error
    if (camera_init_result != ESP_OK) {
        web_server.send(500, "application/json",
                        "{\"ok\":false,\"error\":\"camera_not_initialized\"}");
        return;
    }

    size_t jpegLen = 0;
    uint32_t seq = 0;

    // read latest frame size and sequence safely
    if (xSemaphoreTake(g_frame_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        jpegLen = g_frame_len;
        seq = g_frame_seq;
        xSemaphoreGive(g_frame_mutex);
    }

    // frame age in ms (in case frames are no longer produced)
    uint64_t last_us = 0;

    if (g_http_fps_mutex) xSemaphoreTake(g_http_fps_mutex, portMAX_DELAY);
    last_us = g_prod_last_ts_us;
    if (g_http_fps_mutex) xSemaphoreGive(g_http_fps_mutex);

    uint64_t now_us = esp_timer_get_time();
    uint32_t age_ms = (uint32_t)((now_us - last_us) / 1000ULL);

    // darkness heuristic:
    // very dark image => very small JPEG => jpegLen < DARK_SIZE_THRESHOLD
    bool darkGuess = (jpegLen > 0 && jpegLen < DARK_SIZE_THRESHOLD);

    // build JSON
    String json = "{";
    json += "\"ok\":true";
    json += ",\"frame_seq\":" + String(seq);       // latest frame counter
    json += ",\"jpeg_bytes\":" + String(jpegLen);  // latest JPEG size in bytes
    json += ",\"age_ms\":" + String(age_ms);       // elapsed time since capture
    json += ",\"dark_threshold_bytes\":" + String(DARK_SIZE_THRESHOLD);
    json += ",\"is_dark_guess\":" + String(darkGuess ? "true" : "false");
    json += "}";

    web_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    web_server.send(200, "application/json", json);
}

void setup() {
    WiFi.mode(WIFI_STA);
    esp_wifi_set_max_tx_power(52);

    // Deferred init: stream sync primitives and sensor runtime are created post-WiFi.
    g_stream_sync_ready = false;
    ina226_runtime::set_available(false);

    Serial.begin(115200);
    Serial.setDebugOutput(true);

#ifdef USER_LED_GPIO
    pinMode(USER_LED_GPIO, OUTPUT);
    digitalWrite(USER_LED_GPIO, !USER_LED_ON_LEVEL);
#endif

#ifdef FLASH_LED_GPIO
    pinMode(FLASH_LED_GPIO, OUTPUT);
    analogWriteResolution(8);
    analogWrite(FLASH_LED_GPIO, 0);
#endif

#ifdef ARDUINO_USB_CDC_ON_BOOT
    delay(5000);
#endif

    log_i("Core debug level: %d", CORE_DEBUG_LEVEL);
    log_i("CPU Freq: %d Mhz, %d core(s)", getCpuFrequencyMhz(), ESP.getChipCores());
    log_i("Free heap: %d bytes", ESP.getFreeHeap());
    log_i("SDK version: %s", ESP.getSdkVersion());
    log_i("Board: %s", BOARD_NAME);
    log_i("Starting " APP_TITLE "...");

    if (CAMERA_CONFIG_FB_LOCATION == CAMERA_FB_IN_PSRAM && !psramInit())
        log_e("Failed to initialize PSRAM");

    param_group_camera.addItem(&param_frame_duration);
    param_group_camera.addItem(&param_frame_size);
    param_group_camera.addItem(&param_jpg_quality);
    param_group_camera.addItem(&param_brightness);
    param_group_camera.addItem(&param_contrast);
    param_group_camera.addItem(&param_saturation);
    param_group_camera.addItem(&param_special_effect);
    param_group_camera.addItem(&param_whitebal);
    param_group_camera.addItem(&param_awb_gain);
    param_group_camera.addItem(&param_wb_mode);
    param_group_camera.addItem(&param_exposure_ctrl);
    param_group_camera.addItem(&param_aec2);
    param_group_camera.addItem(&param_ae_level);
    param_group_camera.addItem(&param_aec_value);
    param_group_camera.addItem(&param_gain_ctrl);
    param_group_camera.addItem(&param_agc_gain);
    param_group_camera.addItem(&param_gain_ceiling);
    param_group_camera.addItem(&param_bpc);
    param_group_camera.addItem(&param_wpc);
    param_group_camera.addItem(&param_raw_gma);
    param_group_camera.addItem(&param_lenc);
    param_group_camera.addItem(&param_hmirror);
    param_group_camera.addItem(&param_vflip);
    param_group_camera.addItem(&param_dcw);
    param_group_camera.addItem(&param_colorbar);
    iotWebConf.addParameterGroup(&param_group_camera);

    iotWebConf.getApTimeoutParameter()->visible = true;
    iotWebConf.setConfigSavedCallback(on_config_saved);
    iotWebConf.setWifiConnectionCallback(on_connected);
#ifdef USER_LED_GPIO
    iotWebConf.setStatusPin(USER_LED_GPIO, USER_LED_ON_LEVEL);
#endif
    iotWebConf.init();
    iotWebConf.skipApStartup();

    strcpy(iotWebConf.getApPasswordParameter()->valueBuffer, AP_ADMIN_PASSWORD);
    strcpy(iotWebConf.getWifiParameterGroup()->_wifiSsid, WIFI_SSID);
    strcpy(iotWebConf.getWifiParameterGroup()->_wifiPassword, WIFI_PASSWORD);

    for (auto i = 0; i < 3; i++) {
        camera_init_result = initialize_camera();

        if (camera_init_result == ESP_OK) {
            update_camera_settings();
            break;
        }

        esp_camera_deinit();

        log_e("Failed to initialize camera. Error: 0x%0x. Frame size: %s, frame rate: %d ms, jpeg quality: %d", camera_init_result, param_frame_size.value(), param_frame_duration.value(), param_jpg_quality.value());
        delay(500);
    }

    diagnostics_runtime::init_cpu_idle_hooks();
    btStop();

    Wire.begin(I2C_WIRE_SDA, I2C_WIRE_SCL);

    telegram_runtime::Config telegram_cfg{};
    telegram_cfg.power_provider = telegram_power_provider;
    telegram_runtime::configure(telegram_cfg);

    power_runtime::Config power_cfg{};
    power_cfg.get_cpu_mhz = power_get_cpu_mhz;
    power_cfg.set_cpu_mhz = power_set_cpu_mhz;
    power_cfg.power_on_camera = power_on_camera;
    power_cfg.power_off_camera = power_off_camera;
    power_cfg.set_wifi_ps = wifi_runtime::set_ps;
    power_cfg.is_camera_on = power_camera_is_on;
    power_cfg.eco_mode = &g_eco;
    power_runtime::configure(power_cfg);

    diagnostics_runtime::Config diag_cfg{};
    diag_cfg.rtsp_running = diag_rtsp_running;
    diag_cfg.rtsp_fps = diag_rtsp_fps;
    diag_cfg.rtsp_sessions = diag_rtsp_sessions;
    diagnostics_runtime::configure(diag_cfg);

    web_server.on("/", HTTP_GET, handle_root);
    web_server.on("/config", [] { iotWebConf.handleConfig(); });
    web_server.on("/snapshot", HTTP_GET, handle_snapshot);
    web_server.on("/stream", HTTP_GET, handle_stream);
    web_server.on("/rtsp/stats", HTTP_GET, []() { diagnostics_runtime::handle_rtsp_stats(web_server); });
    web_server.on("/camera/config", HTTP_GET, handle_camera_config);
    web_server.on("/restart", HTTP_GET, handle_restart);
    web_server.on("/sys/stats", HTTP_GET, []() { diagnostics_runtime::handle_sys_stats(web_server); });
    web_server.on("/power/profile", HTTP_GET, []() { power_runtime::handle_profile(web_server); });
    web_server.on("/wifi/sleep", HTTP_GET, []() { wifi_runtime::handle_sleep_status(web_server); });
    web_server.on("/http/fps", HTTP_GET, handle_http_fps);

    web_server.on("/ota/progress", HTTP_GET, []() {
        char buf[32];
        snprintf(buf, sizeof(buf), "{\"pct\":%u}", ota_runtime::progress_percent());
        web_server.send(200, "application/json", buf);
    });

    web_server.on("/ina226", HTTP_GET, []() { ina226_runtime::handle_status(web_server); });
    web_server.on("/ina226/resetcounters", HTTP_GET, []() { ina226_runtime::handle_reset(web_server); });
    web_server.on("/light/status", HTTP_GET, handle_light_status);
    web_server.on("/autosleep", HTTP_GET, handle_auto_sleep);
    web_server.on("/rcwl/status", HTTP_GET, handle_rcwl_status);
    web_server.on("/telegram", HTTP_GET, sendMessageTelegram);
    web_server.on("/sleep", HTTP_GET, handle_sleep);
    web_server.on("/frame/dark", HTTP_GET, handle_frame_dark);

    // xTaskCreatePinnedToCore(
    //     rtsp_task,
    //     "rtsp_task",
    //     4096, nullptr, 3,
    //     &s_rtspTask,
    //     1);

#ifdef FLASH_LED_GPIO
    web_server.on("/flash", HTTP_GET, handle_flash);
#endif
    web_server.onNotFound([]() { iotWebConf.handleNotFound(); });

    ota_runtime::configure(OTA_PASSWORD, OTA_PORT, []() {
        setCpuFrequencyMhz(240);
        delay(20);
        power_off_camera();
        wifi_runtime::set_ps("none");
    });
}

void loop() {
    iotWebConf.doLoop();

    if (camera_server)
        camera_server->doLoop();

    ota_runtime::poll();
}
