#include "src/config/cloud_config.h"
#include "src/config/AppConfig.h"
#include "src/input/ButtonController.h"
#include "src/cloud/CloudTelemetry.h"
#include "src/cloud/CloudUploadController.h"
#include "src/connectivity/ConnectivityManager.h"
#include "src/platform/DeviceIdentity.h"
#include "src/discovery/LocalDiscovery.h"
#include "src/http/LocalWebServer.h"
#include "src/telemetry/TelemetryService.h"
#include "src/telemetry/TelemetryHttpHandler.h"
#include "src/cloud/TelemetryUploader.h"
#include "src/cloud/UploadScheduler.h"
#include "src/platform/WatchdogController.h"
#include "src/config/cloud_ca.h"

// Telemetry acquisition
DeviceIdentity deviceIdentity;
TelemetryService telemetryService;

// Telemetry service via local web server
const LocalDiscoveryService localHttpService = {
    AppConfig::LOCAL_HOSTNAME,
    AppConfig::LOCAL_HTTP_SERVICE_NAME,
    AppConfig::LOCAL_TELEMETRY_PORT,
    AppConfig::LOCAL_HTTP_SERVICE_TXT
};
LocalDiscovery localDiscovery(
    AppConfig::LOCAL_DISCOVERY_RETRY_INTERVAL_MS, localHttpService);
TelemetryHttpHandler telemetryHttpHandler(telemetryService);
LocalWebServer localWebServer(
    telemetryHttpHandler,
    AppConfig::LOCAL_TELEMETRY_PORT,
    AppConfig::LOCAL_WEB_SERVER_RETRY_INTERVAL_MS,
    mbed::callback(&localDiscovery, &LocalDiscovery::update));

// Telemetry delivery via cloud services
CloudTelemetry cloudTelemetry(
    AppConfig::CLOUD_TELEMETRY_URL,
    ISRG_ROOT_X1_CERTIFICATE,
    CLOUD_DEVICE_API_KEY,
    AppConfig::CLOUD_KEY_PLACEHOLDER);
TelemetryUploader telemetryUploader(
    cloudTelemetry,
    telemetryService);
UploadScheduler uploadScheduler(
    AppConfig::CLOUD_UPLOAD_INTERVAL_MS,
    AppConfig::CLOUD_RETRY_INTERVAL_MS);
CloudUploadController cloudUploads(
    uploadScheduler,
    telemetryUploader);

// Input and connectivity
ButtonController buttons(
    USER_BUTTON_A,
    USER_BUTTON_B,
    AppConfig::BUTTON_DEBOUNCE_INTERVAL_MS);
ConnectivityManager connectivity(
    AppConfig::WIFI_STATUS_INTERVAL_MS,
    AppConfig::WIFI_RETRY_INITIAL_MS,
    AppConfig::WIFI_RETRY_MAX_MS,
    AppConfig::NTP_RETRY_INTERVAL_MS);

// Reliability
WatchdogController watchdog(AppConfig::WATCHDOG_TIMEOUT_MS);

// Event handlers for button and connectivity events
// Event driven as next step
void handleButtonEvents(const ButtonEvents &events) {
    if (events.uploadRequested) {
        cloudUploads.requestManualUpload();
    }

    if (events.toggleUploadPause) {
        cloudUploads.togglePaused();
    }
}

void handleConnectivityEvents(const ConnectivityEvents &events) {
    if (events.wifiConnected || events.localAddressChanged) {
        connectivity.printLocalHttpEndpoint("/api/telemetry");
    }

    if (events.wifiConnected) {
        connectivity.printTimeSynchronizationStatus();
    }
}

// Setup and main loop functions
void setup() {
    Serial.begin(115200);
    while (!Serial);

    deviceIdentity.begin();
    buttons.begin();

    watchdog.begin();

    telemetryService.begin(deviceIdentity.get());
    cloudTelemetry.begin();
}

void loop() {
    watchdog.reset();
    ButtonEvents buttonEvents = buttons.update();
    handleButtonEvents(buttonEvents);
    watchdog.reset();
    ConnectivityEvents connectivityEvents = connectivity.update();
    handleConnectivityEvents(connectivityEvents);
    watchdog.reset();

    localWebServer.update(
        connectivity.isWiFiConnected(), connectivity.localIPv4Address());

    watchdog.reset();
    cloudUploads.update(
        connectivity.isWiFiConnected() &&
        connectivity.isTimeSynchronized());
}

