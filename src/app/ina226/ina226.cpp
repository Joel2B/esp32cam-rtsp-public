#include "ina226.h"

#include <WebServer.h>
#include <Wire.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
static const uint8_t INA226_ADDR = 0x40;   // A0/A1 por defecto
static const float INA226_RSHUNT = 0.10f;  // R100 integrado (0.1 Ω)

// For Imax=0.5A, choose Current_LSB ~ 20 uA/LSB for a good range/resolution tradeoff
static const float INA226_CURRENT_LSB = 20e-6f;  // 0.000020 A/LSB
static const uint16_t INA226_CAL = (uint16_t)(0.00512f / (INA226_CURRENT_LSB * INA226_RSHUNT));

struct INA226Reading {
    float vbus_V;      // Voltaje bus (VBUS)
    float vshunt_mV;   // mV en shunt (signado)
    float current_mA;  // mA (signado)
    float power_mW;    // mW (signado)
    uint64_t ts_ms;    // timestamp del muestreo
};

static bool g_ina226_inited = false;
static bool g_ina226_task_ready = false;
static TaskHandle_t s_ina226SensorTask = nullptr;

// Ultima lectura disponible (refrescada por la tarea de muestreo)
static volatile INA226Reading g_last{};

// Accumulators (kept in RTC memory so they can persist across deep sleep)
RTC_DATA_ATTR double g_mAh_net = 0.0;        // (+) discharged, (-) charged
RTC_DATA_ATTR double g_mWh_net = 0.0;        // (+) consumed, (-) charged
RTC_DATA_ATTR double g_mAh_discharge = 0.0;  // discharge only (I>=0)
RTC_DATA_ATTR double g_mAh_charge = 0.0;     // charge only (positive magnitude)
RTC_DATA_ATTR uint64_t g_last_ts_ms = 0;     // for time integration

static portMUX_TYPE g_last_mux = portMUX_INITIALIZER_UNLOCKED;

static bool i2cWrite16(uint8_t addr, uint8_t reg, uint16_t val) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write((uint8_t)(val >> 8));
    Wire.write((uint8_t)(val & 0xFF));

    return Wire.endTransmission() == 0;
}

static bool i2cRead16(uint8_t addr, uint8_t reg, uint16_t& out) {
    Wire.beginTransmission(addr);
    Wire.write(reg);

    if (Wire.endTransmission(false) != 0) return false;  // repeated start
    if (Wire.requestFrom((int)addr, 2) != 2) return false;

    uint8_t msb = Wire.read();
    uint8_t lsb = Wire.read();
    out = ((uint16_t)msb << 8) | lsb;

    return true;
}

// Inicializa INA226 (config + calibracion)
static bool ina226_begin() {
    // Config: promedios 16, conv 1.1ms shunt y bus, modo continuo (shunt+bus)
    uint16_t cfg =
        (0b0100 << 9) |  // AVG=16
        (0b100 << 6) |   // VBUSCT=1.1ms
        (0b100 << 3) |   // VSHCT=1.1ms
        (0b111);         // MODE=Shunt+Bus, continuous

    if (!i2cWrite16(INA226_ADDR, 0x00, cfg)) return false;
    if (!i2cWrite16(INA226_ADDR, 0x05, INA226_CAL)) return false;

    g_ina226_inited = true;
    return true;
}

static bool ina226_read_once(INA226Reading& r) {
    if (!g_ina226_inited && !ina226_begin()) return false;

    uint16_t rawBus = 0, rawShunt = 0, rawCurrent = 0;

    // VBUS (0x02): 1.25 mV/LSB, unsigned
    if (!i2cRead16(INA226_ADDR, 0x02, rawBus)) return false;
    r.vbus_V = (float)rawBus * 1.25e-3f;

    // VSHUNT (0x01): 2.5 uV/LSB, signed
    if (!i2cRead16(INA226_ADDR, 0x01, rawShunt)) return false;
    r.vshunt_mV = (float)((int16_t)rawShunt) * 2.5e-3f;

    // CURRENT (0x04): signed, LSB=Current_LSB
    if (!i2cRead16(INA226_ADDR, 0x04, rawCurrent)) return false;
    r.current_mA = (float)((int16_t)rawCurrent) * (INA226_CURRENT_LSB * 1000.0f);

    // POWER (0x03): signed, LSB=25*Current_LSB (W/LSB) -> mW
    uint16_t tmp;
    if (!i2cRead16(INA226_ADDR, 0x03, tmp)) return false;
    int16_t rawPower = (int16_t)tmp;

    r.power_mW = (float)rawPower * (25.0f * INA226_CURRENT_LSB) * 1000.0f;
    r.ts_ms = millis();

    return true;
}

static void ina226_sensor_task(void* arg) {
    INA226Reading r{};

    for (;;) {
        if (ina226_read_once(r)) {
            // Time integration (if there was a previous sample)
            uint64_t now_ms = r.ts_ms;
            if (g_last_ts_ms == 0) {
                g_last_ts_ms = now_ms;
            } else {
                uint64_t dt_ms = now_ms - g_last_ts_ms;
                g_last_ts_ms = now_ms;

                const double dt_h = (double)dt_ms / 3600000.0;
                const double d_mAh = (double)r.current_mA * dt_h;  // mA*h
                const double d_mWh = (double)r.power_mW * dt_h;    // mW*h

                g_mAh_net += d_mAh;
                g_mWh_net += d_mWh;

                if (r.current_mA >= 0.0f) {
                    g_mAh_discharge += d_mAh;  // discharge
                } else {
                    g_mAh_charge += (-d_mAh);  // store as positive while charging
                }
            }

            // Publish latest sample
            portENTER_CRITICAL(&g_last_mux);
            g_last.vbus_V = r.vbus_V;
            g_last.vshunt_mV = r.vshunt_mV;
            g_last.current_mA = r.current_mA;
            g_last.power_mW = r.power_mW;
            g_last.ts_ms = r.ts_ms;
            portEXIT_CRITICAL(&g_last_mux);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
}  // namespace

namespace ina226_runtime {
void set_available(bool available) {
    g_ina226_task_ready = available;
}

bool is_available() {
    return g_ina226_task_ready && s_ina226SensorTask != nullptr;
}

bool start_task(String* error_out) {
    if (s_ina226SensorTask != nullptr) {
        g_ina226_task_ready = true;
        return true;
    }

    BaseType_t rc = xTaskCreatePinnedToCore(
        ina226_sensor_task,
        "ina226_sampler",
        4096,
        nullptr,
        3,
        &s_ina226SensorTask,
        1);

    if (rc != pdPASS || s_ina226SensorTask == nullptr) {
        g_ina226_task_ready = false;

        if (error_out) *error_out = "xTaskCreatePinnedToCore(ina226_sensor_task)";
        return false;
    }

    g_ina226_task_ready = true;
    return true;
}

void shutdown() {
    uint16_t cfg;
    // Read current configuration and force MODE=000 (shutdown)
    if (i2cRead16(INA226_ADDR, 0x00, cfg)) {  // REG 0x00
        cfg &= ~0x7;                          // MODE bits a 000
        i2cWrite16(INA226_ADDR, 0x00, cfg);
    }

    g_ina226_inited = false;  // force re-init after wake
}

bool latest_power(float* vbus_V, float* current_mA) {
    if (vbus_V == nullptr || current_mA == nullptr) return false;

    portENTER_CRITICAL(&g_last_mux);
    *vbus_V = g_last.vbus_V;
    *current_mA = g_last.current_mA;
    portEXIT_CRITICAL(&g_last_mux);

    return true;
}

void handle_status(WebServer& web_server) {
    if (!is_available()) {
        web_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        web_server.send(503, "application/json", "{\"ok\":false,\"error\":\"ina226_task_not_running\"}");
        return;
    }

    INA226Reading m;
    portENTER_CRITICAL(&g_last_mux);
    m.vbus_V = g_last.vbus_V;
    m.vshunt_mV = g_last.vshunt_mV;
    m.current_mA = g_last.current_mA;
    m.power_mW = g_last.power_mW;
    m.ts_ms = g_last.ts_ms;
    portEXIT_CRITICAL(&g_last_mux);

    const bool charging = (m.current_mA < 0.0f);

    String json = "{";
    json += "\"ok\":" + String(g_ina226_inited ? "true" : "false");
    json += ",\"vbat_V\":" + String(m.vbus_V, 3);
    json += ",\"ishunt_mA\":" + String(m.current_mA, 2);
    json += ",\"psys_mW\":" + String(m.power_mW, 0);
    json += ",\"vshunt_mV\":" + String(m.vshunt_mV, 3);
    json += ",\"rshunt_ohm\":" + String(INA226_RSHUNT, 3);
    json += ",\"status\":\"" + String(charging ? "charging" : "discharging") + "\"";
    json += ",\"mAh_net\":" + String(g_mAh_net, 3);
    json += ",\"mWh_net\":" + String(g_mWh_net, 3);
    json += ",\"mAh_discharge\":" + String(g_mAh_discharge, 3);
    json += ",\"mAh_charge\":" + String(g_mAh_charge, 3);
    json += ",\"uptime_h\":" + String((double)millis() / 3600000.0, 3);
    json += ",\"ts_ms\":" + String((unsigned long)m.ts_ms);
    json += "}";

    web_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    web_server.send(200, "application/json", json);
}

void handle_reset(WebServer& web_server) {
    if (!is_available()) {
        web_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        web_server.send(503, "application/json", "{\"ok\":false,\"error\":\"ina226_task_not_running\"}");
        return;
    }

    g_mAh_net = g_mWh_net = g_mAh_discharge = g_mAh_charge = 0.0;
    g_last_ts_ms = millis();

    web_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    web_server.sendHeader("Refresh", "1");
    web_server.send(200, "application/json", "{\"ok\":true,\"reset\":true}");
}
}  // namespace ina226_runtime
