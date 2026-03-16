#pragma once

#include <Arduino.h>

typedef void (*ota_pre_start_hook_t)();

namespace ota_runtime {
void configure(const char* password, uint16_t port, ota_pre_start_hook_t pre_start_hook);
void poll();
uint8_t progress_percent();
bool started();
}  // namespace ota_runtime
