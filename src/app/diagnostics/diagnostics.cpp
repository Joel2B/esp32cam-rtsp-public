#include "diagnostics.h"

#include <WebServer.h>
#include <esp_freertos_hooks.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
struct MemStats {
    size_t total_ram;
    size_t free_ram;
    size_t largest_ram;

    size_t total_psram;
    size_t free_psram;
    size_t largest_psram;
};

struct CpuStats {
    float core_load[portNUM_PROCESSORS];
};

static diagnostics_runtime::Config g_cfg = {
    .rtsp_running = nullptr,
    .rtsp_fps = nullptr,
    .rtsp_sessions = nullptr,
};

// Idle time accumulators per core (microseconds)
static volatile uint64_t g_idle_us[portNUM_PROCESSORS] = {0, 0};
static volatile uint64_t g_idle_last_ts_us[portNUM_PROCESSORS] = {0, 0};

static MemStats get_mem_stats() {
    MemStats m{};
    m.total_ram = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    m.free_ram = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    m.largest_ram = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    m.total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    m.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    m.largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    return m;
}

static bool idle_hook_core0() {
    uint64_t now = esp_timer_get_time();
    uint64_t last = g_idle_last_ts_us[0];
    if (last != 0) g_idle_us[0] += (now - last);
    g_idle_last_ts_us[0] = now;
    return true;
}

#if portNUM_PROCESSORS > 1
static bool idle_hook_core1() {
    uint64_t now = esp_timer_get_time();
    uint64_t last = g_idle_last_ts_us[1];
    if (last != 0) g_idle_us[1] += (now - last);
    g_idle_last_ts_us[1] = now;
    return true;
}
#endif

static CpuStats sample_cpu_load(uint32_t interval_us) {
    uint64_t idle0_a = g_idle_us[0];
#if portNUM_PROCESSORS > 1
    uint64_t idle1_a = g_idle_us[1];
#endif
    uint64_t t0 = esp_timer_get_time();

    uint32_t ms = interval_us / 1000;
    vTaskDelay(pdMS_TO_TICKS(ms));

    uint64_t t1 = esp_timer_get_time();
    uint64_t idle0_b = g_idle_us[0];
#if portNUM_PROCESSORS > 1
    uint64_t idle1_b = g_idle_us[1];
#endif

    uint64_t real_dt = (t1 - t0);
    CpuStats s{};
    float idle_frac0 = real_dt ? (float)(idle0_b - idle0_a) / (float)real_dt : 0.0f;
    s.core_load[0] = 100.0f * (1.0f - idle_frac0);
#if portNUM_PROCESSORS > 1
    float idle_frac1 = real_dt ? (float)(idle1_b - idle1_a) / (float)real_dt : 0.0f;
    s.core_load[1] = 100.0f * (1.0f - idle_frac1);
#endif
    return s;
}
}  // namespace

namespace diagnostics_runtime {
void configure(const Config& config) {
    g_cfg = config;
}

void init_cpu_idle_hooks() {
    esp_register_freertos_idle_hook_for_cpu(idle_hook_core0, 0);
#if portNUM_PROCESSORS > 1
    esp_register_freertos_idle_hook_for_cpu(idle_hook_core1, 1);
#endif
}

void handle_rtsp_stats(WebServer& web_server) {
    const bool running = (g_cfg.rtsp_running != nullptr) ? g_cfg.rtsp_running() : false;
    if (!running) {
        web_server.send(503, "application/json", "{\"error\":\"rtsp not running\"}");
        return;
    }

    float fps = (g_cfg.rtsp_fps != nullptr) ? g_cfg.rtsp_fps() : 0.0f;
    size_t sessions = (g_cfg.rtsp_sessions != nullptr) ? g_cfg.rtsp_sessions() : 0;
    String json = String("{\"fps\":") + String(fps, 2) + ",\"sessions\":" + String(sessions) + "}";
    web_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    web_server.send(200, "application/json", json);
}

void handle_sys_stats(WebServer& web_server) {
    CpuStats cpu = sample_cpu_load(200000);
    MemStats mem = get_mem_stats();

    auto toMB = [](size_t bytes) {
        return (float)bytes / (1024.0f * 1024.0f);
    };

    String json = "{";
    json += "\"cpu\":{";
    json += "\"cores\":" + String(portNUM_PROCESSORS) + ",";
    json += "\"core0\":" + String(cpu.core_load[0], 1);
#if portNUM_PROCESSORS > 1
    json += ",\"core1\":" + String(cpu.core_load[1], 1);
#endif
    json += "},";

    json += "\"ram\":{";
    json += "\"total\":" + String(toMB(mem.total_ram), 2) + ",";
    json += "\"free\":" + String(toMB(mem.free_ram), 2) + ",";
    json += "\"largest\":" + String(toMB(mem.largest_ram), 2);
    json += "},";

    json += "\"psram\":{";
    json += "\"total\":" + String(toMB(mem.total_psram), 2) + ",";
    json += "\"free\":" + String(toMB(mem.free_psram), 2) + ",";
    json += "\"largest\":" + String(toMB(mem.largest_psram), 2);
    json += "}";
    json += "}";

    web_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    web_server.send(200, "application/json", json);
}
}  // namespace diagnostics_runtime
