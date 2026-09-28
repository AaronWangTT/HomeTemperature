#include <ArduinoMDNS.h>

#include "src/config/cloud_config.h"
#include "src/config/AppConfig.h"
#include "src/input/ButtonController.h"
#include "src/cloud/CloudTelemetry.h"
#include "src/cloud/CloudUploadController.h"
#include "src/connectivity/ConnectivityManager.h"
#include "src/platform/DeviceIdentity.h"
#include "src/discovery/LocalDiscovery.h"
#include "src/http/LocalWebServer.h"
#include "src/ota/LocalOtaController.h"
#include "src/ota/LocalOtaHttpHandler.h"
#include "src/ota/LocalOtaPlatform.h"
#include "src/ota/NetworkMaintenanceCoordinator.h"
#include "src/ota/OtaRebootCoordinator.h"
#include "src/telemetry/TelemetryService.h"
#include "src/telemetry/TelemetryHttpHandler.h"
#include "src/cloud/TelemetryUploader.h"
#include "src/cloud/UploadScheduler.h"
#include "src/platform/WatchdogController.h"
#include "src/config/cloud_ca.h"
#include "src/config/ota_public_key.h"

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
NetworkMaintenanceCoordinator networkMaintenance;
Az3166OtaCore otaCore;
Az3166OtaEntropy otaEntropy;
Az3166OtaDisplay otaDisplay;
LocalOtaController otaController(
    otaCore,
    otaEntropy,
    otaDisplay,
    networkMaintenance,
    HOME_TEMPERATURE_OTA_PUBLIC_KEY_DER,
    HOME_TEMPERATURE_OTA_PUBLIC_KEY_DER_SIZE,
    AppConfig::OTA_PRODUCT_ID,
    AppConfig::OTA_BOARD_ID,
    AppConfig::OTA_FIRMWARE_VERSION,
    millis);
LocalOtaHttpHandler localHttpHandler(telemetryHttpHandler, otaController);
const LocalHttpStreamingLimits otaStreamingLimits = {
    AppConfig::OTA_MAX_PACKAGE_SIZE,
    AppConfig::OTA_UPLOAD_IDLE_TIMEOUT_MS,
    AppConfig::OTA_UPLOAD_TOTAL_TIMEOUT_MS
};
LocalWebServer localWebServer(
    localHttpHandler,
    localHttpHandler,
    otaStreamingLimits,
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
    telemetryUploader,
    networkMaintenance);

// Input and connectivity
ButtonController buttons(
    USER_BUTTON_A,
    USER_BUTTON_B,
    AppConfig::BUTTON_DEBOUNCE_INTERVAL_MS,
    AppConfig::OTA_BUTTON_HOLD_INTERVAL_MS);
ConnectivityManager connectivity(
    AppConfig::WIFI_STATUS_INTERVAL_MS,
    AppConfig::WIFI_RETRY_INITIAL_MS,
    AppConfig::WIFI_RETRY_MAX_MS,
    AppConfig::NTP_RETRY_INTERVAL_MS);

// Reliability
WatchdogController watchdog(AppConfig::WATCHDOG_TIMEOUT_MS);
OtaRebootCoordinator otaReboot(AppConfig::OTA_REBOOT_DELAY_MS);

// Event handlers for button and connectivity events
// Event driven as next step
void handleButtonEvents(const ButtonEvents &events) {
    if (events.otaRequested) {
        otaController.openChallenge();
        return;
    }
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
    Screen.init();

    watchdog.begin();

    telemetryService.begin(deviceIdentity.get());
    cloudTelemetry.begin();
    otaController.begin();
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
    otaController.update(localWebServer.state().generation);

    if (otaController.takeRebootRequest()) {
        otaReboot.schedule(millis());
    }
    if (otaReboot.due(millis())) {
        NVIC_SystemReset();
    }

    watchdog.reset();
    if (otaReboot.cloudAllowed()) {
        cloudUploads.update(
            connectivity.isWiFiConnected() &&
            connectivity.isTimeSynchronized());
    }
}
