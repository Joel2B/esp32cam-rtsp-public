#include "telegram.h"
#include "config/secrets.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ctype.h>

namespace telegram_runtime {
namespace {
Config g_cfg = {
    TELEGRAM_MESSAGE_BOT_TOKEN,
    TELEGRAM_MESSAGE_CHAT_ID,
    TELEGRAM_PHOTO_BOT_TOKEN,
    TELEGRAM_PHOTO_CHAT_ID,
    nullptr};

static String getDateTimeString() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return "NO_TIME";
    }

    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(buffer);
}

static String urlEncode(const String& s) {
    String out;
    out.reserve(s.length() * 1.2);
    const char* hex = "0123456789ABCDEF";
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else if (c == ' ') {
            out += "%20";
        } else {
            out += '%';
            out += hex[(uint8_t)c >> 4];
            out += hex[(uint8_t)c & 0x0F];
        }
    }
    return out;
}
}  // namespace

void configure(const Config& cfg) {
    if (cfg.message_bot_token != nullptr) g_cfg.message_bot_token = cfg.message_bot_token;
    if (cfg.message_chat_id != nullptr) g_cfg.message_chat_id = cfg.message_chat_id;
    if (cfg.photo_bot_token != nullptr) g_cfg.photo_bot_token = cfg.photo_bot_token;
    if (cfg.photo_chat_id != nullptr) g_cfg.photo_chat_id = cfg.photo_chat_id;
    g_cfg.power_provider = cfg.power_provider;
}

String send(const String& text, const String& parse_mode) {
    PowerTelemetry m{};
    if (g_cfg.power_provider) {
        (void)g_cfg.power_provider(&m);
    }

    String ina226 = "[vbat_V: " + String(m.vbus_V, 3) + ", ishunt_mA: " + String(m.current_mA, 2) + "]";
    String dateTime = "[" + getDateTimeString() + "]";
    String encodedText = dateTime + ina226 + " " + urlEncode(text);

    String endpoint = "https://api.telegram.org/bot" + String(g_cfg.message_bot_token) + "/sendMessage";
    String url = endpoint + "?chat_id=" + String(g_cfg.message_chat_id) + "&text=" + encodedText;
    if (parse_mode.length() > 0) {
        url += "&parse_mode=" + parse_mode;
    }

    WiFiClientSecure client;
    HTTPClient https;
    client.setInsecure();
    https.setConnectTimeout(2000);
    https.setTimeout(4000);

    String response = "";
    if (!https.begin(client, url)) {
        response = "[TG] begin() failed";
    }

    int code = https.GET();
    if (code > 0) {
        response = String(code);
        if (code == HTTP_CODE_OK) {
            String payload = https.getString();
            response = payload.c_str();
            https.end();
        } else {
            response = https.getString().c_str();
        }
    } else {
        response = https.errorToString(code).c_str();
    }

    https.end();
    return response;
}

bool sendPhotoBuffer(const uint8_t* data, size_t len, const String& filename, const String& caption) {
    if (!data || len == 0) return false;

    WiFiClientSecure client;
    client.setInsecure();

    const char* host = "api.telegram.org";
    const uint16_t port = 443;
    if (!client.connect(host, port)) {
        Serial.println("[TG] Could not connect to Telegram");
        return false;
    }

    String boundary = "----ESP32Boundary7MA4YWxkTrZu0gW";
    String contentType = "multipart/form-data; boundary=" + boundary;

    String partChat =
        "--" + boundary +
        "\r\n"
        "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" +
        String(g_cfg.photo_chat_id) + "\r\n";

    String partCaption = "";
    if (caption.length()) {
        partCaption =
            "--" + boundary +
            "\r\n"
            "Content-Disposition: form-data; name=\"caption\"\r\n\r\n" +
            caption + "\r\n";
    }

    String partFileHeader =
        "--" + boundary +
        "\r\n"
        "Content-Disposition: form-data; name=\"photo\"; filename=\"" +
        filename +
        "\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n";

    String partEnd = "\r\n--" + boundary + "--\r\n";

    size_t contentLen =
        partChat.length() + partCaption.length() + partFileHeader.length() + len + partEnd.length();

    String endpoint = "/bot" + String(g_cfg.photo_bot_token) + "/sendPhoto";

    client.printf("POST %s HTTP/1.1\r\n", endpoint.c_str());
    client.printf("Host: %s\r\n", host);
    client.print("User-Agent: ESP32S3\r\n");
    client.printf("Content-Type: %s\r\n", contentType.c_str());
    client.printf("Content-Length: %u\r\n", (unsigned)contentLen);
    client.print("Connection: close\r\n\r\n");

    client.print(partChat);
    if (partCaption.length()) client.print(partCaption);
    client.print(partFileHeader);

    const uint8_t* p = data;
    size_t left = len;
    while (left > 0) {
        size_t chunk = left > 4096 ? 4096 : left;
        size_t w = client.write(p, chunk);
        if (w == 0) {
            Serial.println("[TG] write() returned 0");
            break;
        }
        p += w;
        left -= w;
    }

    client.print(partEnd);
    client.setTimeout(4000);
    String status = client.readStringUntil('\n');
    Serial.printf("[TG] status: %s", status.c_str());

    String resp;
    while (client.connected() || client.available()) {
        String line = client.readStringUntil('\n');
        resp += line + "\n";
    }
    Serial.println(resp);

    return status.indexOf("200") >= 0;
}
}  // namespace telegram_runtime
