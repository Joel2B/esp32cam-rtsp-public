#if __has_include("secrets.local.h")
#include "secrets.local.h"
#else
#error "Missing src/config/secrets.local.h. Create it with WIFI_SSID, WIFI_PASSWORD, AP_ADMIN_PASSWORD, OTA_PASSWORD, TELEGRAM_MESSAGE_BOT_TOKEN, TELEGRAM_MESSAGE_CHAT_ID (and optional OTA_PORT, TELEGRAM_PHOTO_BOT_TOKEN, TELEGRAM_PHOTO_CHAT_ID)."
#endif

#ifndef WIFI_SSID
#error "WIFI_SSID is not defined in secrets.local.h"
#endif

#ifndef WIFI_PASSWORD
#error "WIFI_PASSWORD is not defined in secrets.local.h"
#endif

#ifndef AP_ADMIN_PASSWORD
#error "AP_ADMIN_PASSWORD is not defined in secrets.local.h"
#endif

#ifndef OTA_PASSWORD
#error "OTA_PASSWORD is not defined in secrets.local.h"
#endif

#ifndef TELEGRAM_MESSAGE_BOT_TOKEN
#error "TELEGRAM_MESSAGE_BOT_TOKEN is not defined in secrets.local.h"
#endif

#ifndef TELEGRAM_MESSAGE_CHAT_ID
#error "TELEGRAM_MESSAGE_CHAT_ID is not defined in secrets.local.h"
#endif

#ifndef TELEGRAM_PHOTO_BOT_TOKEN
#define TELEGRAM_PHOTO_BOT_TOKEN TELEGRAM_MESSAGE_BOT_TOKEN
#endif

#ifndef TELEGRAM_PHOTO_CHAT_ID
#define TELEGRAM_PHOTO_CHAT_ID TELEGRAM_MESSAGE_CHAT_ID
#endif

#ifndef OTA_PORT
#define OTA_PORT 3232
#endif
