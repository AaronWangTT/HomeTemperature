#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stddef.h>
#include <stdint.h>

namespace AppConfig {

#ifndef HOME_TEMPERATURE_CLOUD_ENDPOINT
#define HOME_TEMPERATURE_CLOUD_ENDPOINT \
    "https://telemetry.example.com/api/v1/telemetry"
#endif

static const char TELEMETRY_REQUEST[] = "GET /api/telemetry ";
static const size_t TELEMETRY_PAYLOAD_SIZE = 160;
static const uint16_t LOCAL_TELEMETRY_PORT = 80;
static const unsigned long LOCAL_WEB_SERVER_RETRY_INTERVAL_MS = 5000UL;

static const char CLOUD_TELEMETRY_URL[] = HOME_TEMPERATURE_CLOUD_ENDPOINT;
static const char CLOUD_KEY_PLACEHOLDER[] = "replace-with-device-key";
static const unsigned long CLOUD_UPLOAD_INTERVAL_MS = 300000UL;
static const unsigned long CLOUD_RETRY_INTERVAL_MS = 15000UL;

static const unsigned long WIFI_STATUS_INTERVAL_MS = 1000UL;
static const unsigned long WIFI_RETRY_INITIAL_MS = 5000UL;
static const unsigned long WIFI_RETRY_MAX_MS = 60000UL;
static const unsigned long NTP_RETRY_INTERVAL_MS = 60000UL;

static const unsigned long BUTTON_DEBOUNCE_INTERVAL_MS = 50UL;
static const float WATCHDOG_TIMEOUT_MS = 30000.0f;

}  // namespace AppConfig

#endif