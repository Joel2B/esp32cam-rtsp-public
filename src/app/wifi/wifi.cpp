#include "wifi.h"

#include <WebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>

namespace {
const char* ps_to_str(wifi_ps_type_t t) {
    switch (t) {
        case WIFI_PS_NONE:
            return "none";
        case WIFI_PS_MIN_MODEM:
            return "min";
        case WIFI_PS_MAX_MODEM:
            return "max";
        default:
            return "unknown";
    }
}

void print_wifi_sleep_status() {
    wifi_ps_type_t t;
    esp_err_t err = esp_wifi_get_ps(&t);
    if (err == ESP_OK) {
        Serial.printf("[WiFi] power save mode: %s  (active=%s)\n",
                      ps_to_str(t), (t != WIFI_PS_NONE ? "true" : "false"));
    } else {
        Serial.printf("[WiFi] get_ps error: 0x%X\n", (unsigned)err);
    }
}
}  // namespace

namespace wifi_runtime {
void set_ps(const char* mode) {
    if (!strcmp(mode, "none")) {
        WiFi.setSleep(false);
        esp_wifi_set_ps(WIFI_PS_NONE);
    } else if (!strcmp(mode, "min")) {
        WiFi.setSleep(true);
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    } else if (!strcmp(mode, "max")) {
        WiFi.setSleep(true);
        esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    }

    print_wifi_sleep_status();
}

void handle_sleep_status(WebServer& web_server) {
    wifi_ps_type_t t;
    bool ok = (esp_wifi_get_ps(&t) == ESP_OK);
    String mode = ok ? ps_to_str(t) : "unknown";
    bool active = ok ? (t != WIFI_PS_NONE) : false;
    String json = String("{\"ok\":") + (ok ? "true" : "false") + ",\"active\":" + (active ? "true" : "false") + ",\"mode\":\"" + mode + "\"}";
    web_server.send(200, "application/json", json);
}
}  // namespace wifi_runtime
