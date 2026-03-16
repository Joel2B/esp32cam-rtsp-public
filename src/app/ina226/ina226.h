#pragma once

#include <Arduino.h>

class WebServer;

namespace ina226_runtime {
void set_available(bool available);
bool is_available();
bool start_task(String* error_out = nullptr);
void shutdown();
void handle_status(WebServer& web_server);
void handle_reset(WebServer& web_server);
bool latest_power(float* vbus_V, float* current_mA);
}  // namespace ina226_runtime

