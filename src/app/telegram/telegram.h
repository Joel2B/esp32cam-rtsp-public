#pragma once

#include <Arduino.h>

namespace telegram_runtime {
struct PowerTelemetry {
    float vbus_V;
    float current_mA;
};

typedef bool (*power_telemetry_provider_t)(PowerTelemetry* out);

struct Config {
    const char* message_bot_token;
    const char* message_chat_id;
    const char* photo_bot_token;
    const char* photo_chat_id;
    power_telemetry_provider_t power_provider;
};

void configure(const Config& cfg);
String send(const String& text, const String& parse_mode = "");
bool sendPhotoBuffer(const uint8_t* data, size_t len,
                     const String& filename = "frame.jpg",
                     const String& caption = "");
}  // namespace telegram_runtime

