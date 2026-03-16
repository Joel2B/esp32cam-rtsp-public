#include "power.h"

#include <WebServer.h>

namespace {
static power_runtime::Config g_cfg = {
    .get_cpu_mhz = nullptr,
    .set_cpu_mhz = nullptr,
    .power_on_camera = nullptr,
    .power_off_camera = nullptr,
    .set_wifi_ps = nullptr,
    .is_camera_on = nullptr,
    .eco_mode = nullptr,
};
}  // namespace

namespace power_runtime {
void configure(const Config& config) {
    g_cfg = config;
}

void handle_profile(WebServer& web_server) {
    if (g_cfg.get_cpu_mhz == nullptr ||
        g_cfg.set_cpu_mhz == nullptr ||
        g_cfg.power_on_camera == nullptr ||
        g_cfg.power_off_camera == nullptr ||
        g_cfg.set_wifi_ps == nullptr ||
        g_cfg.is_camera_on == nullptr ||
        g_cfg.eco_mode == nullptr) {
        web_server.send(500, "application/json", "{\"ok\":false,\"error\":\"power_runtime_not_configured\"}");
        return;
    }

    String mode = web_server.hasArg("mode") ? web_server.arg("mode") : "";
    String wifi_mode = web_server.hasArg("wifi") ? web_server.arg("wifi") : "";
    String ps = wifi_mode.length() ? wifi_mode : "min";

    int req_mhz = -1;
    if (web_server.hasArg("mhz")) req_mhz = web_server.arg("mhz").toInt();
    int before_mhz = g_cfg.get_cpu_mhz();
    int after_mhz = before_mhz;
    bool applied = false;
    String cpu_err = "";

    auto apply_cpu = [&](int mhz) {
        if (mhz == 80 || mhz == 160 || mhz == 240) {
            if (mhz != before_mhz) {
                g_cfg.set_cpu_mhz(mhz);
                delay(20);
            }

            after_mhz = g_cfg.get_cpu_mhz();
            applied = (after_mhz == mhz);
            if (!applied) cpu_err = "apply_failed";
        } else {
            cpu_err = "unsupported_mhz";
        }
    };

    if (mode == "eco") {
        if (req_mhz == -1) req_mhz = 80;

        apply_cpu(req_mhz);

        g_cfg.power_off_camera();
        g_cfg.set_wifi_ps(ps.c_str());

        *g_cfg.eco_mode = true;

        String json = "{";
        json += "\"ok\":true,\"profile\":\"eco\",";
        json += "\"camera\":\"off\",";
        json += "\"cpu\":{\"requested\":" + String(req_mhz) + ",\"before\":" + String(before_mhz) + ",\"after\":" + String(after_mhz) + ",\"applied\":" + String(applied ? "true" : "false") + (cpu_err.length() ? ",\"error\":\"" + cpu_err + "\"" : "") + "}";
        json += "}";
        web_server.send(200, "application/json", json);
        return;
    }

    if (mode == "normal") {
        if (req_mhz == -1) req_mhz = 240;
        apply_cpu(req_mhz);

        g_cfg.power_on_camera();
        g_cfg.set_wifi_ps("none");

        *g_cfg.eco_mode = false;

        bool cam_on = g_cfg.is_camera_on();

        String json = "{";
        json += "\"ok\":true,\"profile\":\"normal\",";
        json += "\"camera\":\"" + String(cam_on ? "on" : "off") + "\",";
        json += "\"cpu\":{\"requested\":" + String(req_mhz) + ",\"before\":" + String(before_mhz) + ",\"after\":" + String(after_mhz) + ",\"applied\":" + String(applied ? "true" : "false") + (cpu_err.length() ? ",\"error\":\"" + cpu_err + "\"" : "") + "}";
        json += "}";
        web_server.send(200, "application/json", json);
        return;
    }

    if (req_mhz > 0) {
        apply_cpu(req_mhz);
        String json = "{";
        json += "\"ok\":" + String(applied ? "true" : "false") + ",";
        json += "\"cpu\":{\"requested\":" + String(req_mhz) + ",\"before\":" + String(before_mhz) + ",\"after\":" + String(after_mhz) + ",\"applied\":" + String(applied ? "true" : "false") + (cpu_err.length() ? ",\"error\":\"" + cpu_err + "\"" : "") + "}";
        json += "}";
        web_server.send(200, "application/json", json);
        return;
    }

    if (mode == "view") {
        String json = "{";
        json += "\"ok\":" + String(applied ? "true" : "false") + ",";
        json += "\"cpu\":{\"requested\":" + String(req_mhz) + ",\"before\":" + String(before_mhz) + ",\"after\":" + String(after_mhz) + ",\"applied\":" + String(applied ? "true" : "false") + (cpu_err.length() ? ",\"error\":\"" + cpu_err + "\"" : "") + "}";
        json += "}";
        web_server.send(200, "application/json", json);
        return;
    }

    web_server.send(400, "application/json",
                    "{\"ok\":false,\"error\":\"use /power/profile?mode=eco|normal[&mhz=80|160|240] or /power/profile?mhz=...\"}");
}
}  // namespace power_runtime
