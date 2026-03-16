#pragma once

#include <Arduino.h>

class WebServer;

namespace diagnostics_runtime {
struct Config {
    bool (*rtsp_running)();
    float (*rtsp_fps)();
    size_t (*rtsp_sessions)();
};

void configure(const Config& config);
void init_cpu_idle_hooks();
void handle_rtsp_stats(WebServer& web_server);
void handle_sys_stats(WebServer& web_server);
}  // namespace diagnostics_runtime

