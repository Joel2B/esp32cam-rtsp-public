#include "ota.h"

#include <ArduinoOTA.h>
#include <WiFi.h>

namespace ota_runtime {
namespace {
const char* g_password = nullptr;
uint16_t g_port = 3232;
ota_pre_start_hook_t g_pre_start_hook = nullptr;
bool g_started = false;
volatile uint8_t g_percent = 0;
}  // namespace

void configure(const char* password, uint16_t port, ota_pre_start_hook_t pre_start_hook) {
    g_password = password;
    g_port = port;
    g_pre_start_hook = pre_start_hook;
}

static void begin_if_needed() {
    if (g_started || !WiFi.isConnected()) return;

    String host = String("cam-") + WiFi.macAddress();
    host.replace(":", "");
    host.toLowerCase();

    ArduinoOTA.setHostname(host.c_str());
    ArduinoOTA.setPort(g_port);

    if (g_password != nullptr) {
        ArduinoOTA.setPassword(g_password);
    }

    ArduinoOTA.onStart([]() {
        if (g_pre_start_hook) g_pre_start_hook();
    });

    ArduinoOTA.onProgress([](unsigned int written, unsigned int total) {
        if (total == 0) return;

        auto toKiB = [](uint32_t b) { return (b + 1023u) >> 10; };
        uint8_t pct = (uint8_t)((uint64_t)written * 100 / total);

        static uint8_t last = 255;
        static uint32_t lastLog = 0;

        if (pct != last && (millis() - lastLog) > 80) {
            last = pct;
            lastLog = millis();
            g_percent = pct;

            uint32_t wKiB = toKiB(written);
            uint32_t tKiB = toKiB(total);

            Serial.printf("\r[OTA] %3u%%  %u/%u KiB", pct, wKiB, tKiB);
            Serial.flush();
        }

        taskYIELD();
    });

    ArduinoOTA.onEnd([]() {
        Serial.println("[OTA] end -> restart");
        ESP.restart();
    });

    ArduinoOTA.onError([](ota_error_t e) {
        Serial.printf("[OTA] error %u\n", e);
    });

    ArduinoOTA.begin();
    g_started = true;
    Serial.printf("[OTA] STA=%s port=%u\n", WiFi.localIP().toString().c_str(), (unsigned)g_port);
}

void poll() {
    begin_if_needed();
    if (g_started) ArduinoOTA.handle();
}

uint8_t progress_percent() {
    return g_percent;
}

bool started() {
    return g_started;
}
}  // namespace ota_runtime
