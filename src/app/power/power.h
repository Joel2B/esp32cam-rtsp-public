#pragma once

#include <Arduino.h>

class WebServer;

namespace power_runtime {
struct Config {
    int (*get_cpu_mhz)();
    void (*set_cpu_mhz)(int mhz);
    void (*power_on_camera)();
    void (*power_off_camera)();
    void (*set_wifi_ps)(const char* mode);
    bool (*is_camera_on)();
    bool* eco_mode;
};

void configure(const Config& config);
void handle_profile(WebServer& web_server);
}  // namespace power_runtime

