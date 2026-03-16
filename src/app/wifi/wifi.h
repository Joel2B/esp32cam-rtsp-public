#pragma once

#include <Arduino.h>

class WebServer;

namespace wifi_runtime {
void set_ps(const char* mode);
void handle_sleep_status(WebServer& web_server);
}  // namespace wifi_runtime

