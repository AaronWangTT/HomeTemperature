#include "cloud_config.h"
#include "src/AppConfig.h"
#include "src/ButtonController.h"
#include "src/CloudTelemetry.h"
#include "src/CloudUploadController.h"
#include "src/ConnectivityManager.h"
#include "src/DeviceIdentity.h"
#include "src/LocalWebServer.h"
#include "src/TelemetryService.h"
#include "src/TelemetryUploader.h"
#include "src/UploadScheduler.h"
#include "src/WatchdogController.h"
#include "cloud_ca.h"

// Telemetry acquisition
DeviceIdentity deviceIdentity;
TelemetryService telemetryService;

// Telemetry delivery
LocalWebServer localWebServer(
    telemetryService,
    AppConfig::LOCAL_TELEMETRY_PORT,
    AppConfig::LOCAL_WEB_SERVER_RETRY_INTERVAL_MS);
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

void handleButtonEvents(const ButtonEvents &events) {
    if (events.uploadRequested) {
        cloudUploads.requestManualUpload();
    }

    if (events.toggleUploadPause) {
        cloudUploads.togglePaused();
    }
}

void handleConnectivityEvents(const ConnectivityEvents &events) {
    if (!events.wifiConnected) {
        return;
    }

    connectivity.printLocalHttpEndpoint("/api/telemetry");
    connectivity.printTimeSynchronizationStatus();
}

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

    localWebServer.poll(connectivity.isWiFiConnected());

    watchdog.reset();
    cloudUploads.update(
        connectivity.isWiFiConnected() &&
        connectivity.isTimeSynchronized());
}

