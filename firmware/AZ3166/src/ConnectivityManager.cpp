#include "ConnectivityManager.h"

#include <Arduino.h>
#include <AZ3166WiFi.h>
#include "SystemTime.h"

namespace {

uint32_t platformCurrentTime() {
    return millis();
}

bool platformIsWiFiConnected() {
    return WiFi.status() == WL_CONNECTED;
}

void platformDisconnectWiFi() {
    WiFi.disconnect();
}

bool platformConnectWiFi() {
    return WiFi.begin() == WL_CONNECTED;
}

bool platformIsTimeSynchronized() {
    return IsTimeSynced() == 0;
}

void platformSynchronizeTime() {
    SyncTime();
}

}  // namespace

ConnectivityManager::ConnectivityManager(
    uint32_t wifiStatusIntervalMs,
    uint32_t wifiRetryInitialMs,
    uint32_t wifiRetryMaxMs,
    uint32_t ntpRetryIntervalMs)
    : ConnectivityManager(
          wifiStatusIntervalMs,
          wifiRetryInitialMs,
          wifiRetryMaxMs,
          ntpRetryIntervalMs,
          defaultOperations()) {
}

ConnectivityManager::ConnectivityManager(
    uint32_t wifiStatusIntervalMs,
    uint32_t wifiRetryInitialMs,
    uint32_t wifiRetryMaxMs,
    uint32_t ntpRetryIntervalMs,
    const ConnectivityOperations &operations)
    : wifiStatusIntervalMs_(wifiStatusIntervalMs),
      wifiRetryInitialMs_(wifiRetryInitialMs),
      wifiRetryMaxMs_(wifiRetryMaxMs),
      ntpRetryIntervalMs_(ntpRetryIntervalMs),
      operations_(operations),
      wifiConnected_(false),
      timeSynchronized_(false),
      lastWiFiStatusCheck_(0),
      lastWiFiAttempt_(0),
      wifiRetryDelay_(0),
      lastNtpAttempt_(0) {
}

ConnectivityEvents ConnectivityManager::update() {
    ConnectivityEvents events = {false, false, false};
    maintainWiFi(events);
    maintainTimeSynchronization(events);
    return events;
}

bool ConnectivityManager::isWiFiConnected() const {
    return wifiConnected_;
}

bool ConnectivityManager::isTimeSynchronized() const {
    return timeSynchronized_;
}

void ConnectivityManager::printLocalHttpEndpoint(
    const char *path) const {
    if (!wifiConnected_ || path == NULL) {
        return;
    }

    Serial.print("Telemetry endpoint: http://");
    Serial.print(WiFi.localIP());
    Serial.println(path);
}

void ConnectivityManager::printTimeSynchronizationStatus() const {
    if (timeSynchronized_) {
        Serial.println("System time synchronized");
    } else {
        Serial.println("NTP sync failed; retrying in 60 seconds");
    }
}

uint32_t ConnectivityManager::nextRetryDelay(
    uint32_t currentDelay,
    uint32_t initialDelay,
    uint32_t maximumDelay) {
    if (currentDelay == 0) {
        return initialDelay;
    }
    if (currentDelay < maximumDelay / 2) {
        return currentDelay * 2;
    }
    return maximumDelay;
}

ConnectivityOperations ConnectivityManager::defaultOperations() {
    ConnectivityOperations operations = {
        platformCurrentTime,
        platformIsWiFiConnected,
        platformDisconnectWiFi,
        platformConnectWiFi,
        platformIsTimeSynchronized,
        platformSynchronizeTime
    };
    return operations;
}

void ConnectivityManager::maintainWiFi(ConnectivityEvents &events) {
    uint32_t now = operations_.currentTime();

    if (wifiConnected_) {
        if (now - lastWiFiStatusCheck_ < wifiStatusIntervalMs_) {
            return;
        }

        lastWiFiStatusCheck_ = now;
        if (operations_.isWiFiConnected()) {
            return;
        }

        Serial.println("Wi-Fi connection lost");
        wifiConnected_ = false;
        timeSynchronized_ = false;
        wifiRetryDelay_ = 0;
        events.wifiDisconnected = true;
        return;
    }

    if (wifiRetryDelay_ > 0 &&
        now - lastWiFiAttempt_ < wifiRetryDelay_) {
        return;
    }

    attemptWiFiConnection(events);
}

void ConnectivityManager::attemptWiFiConnection(
    ConnectivityEvents &events) {
    lastWiFiAttempt_ = operations_.currentTime();
    Serial.println("Connecting to configured Wi-Fi...");

    operations_.disconnectWiFi();
    if (operations_.connectWiFi()) {
        handleWiFiConnected(events);
        return;
    }

    wifiConnected_ = false;
    timeSynchronized_ = false;
    lastWiFiAttempt_ = operations_.currentTime();
    scheduleWiFiRetry();
}

void ConnectivityManager::handleWiFiConnected(
    ConnectivityEvents &events) {
    wifiConnected_ = true;
    wifiRetryDelay_ = 0;

    uint32_t now = operations_.currentTime();
    lastWiFiStatusCheck_ = now;
    lastNtpAttempt_ = now;
    timeSynchronized_ = operations_.isTimeSynchronized();

    printConnectionDetails();
    events.wifiConnected = true;
    events.timeSynchronized = timeSynchronized_;
}

void ConnectivityManager::printConnectionDetails() const {
    Serial.print("Connected to Wi-Fi: ");
    Serial.print(WiFi.SSID());
    Serial.print(". IP address: ");
    Serial.println(WiFi.localIP());
}

void ConnectivityManager::scheduleWiFiRetry() {
    wifiRetryDelay_ = nextRetryDelay(
        wifiRetryDelay_,
        wifiRetryInitialMs_,
        wifiRetryMaxMs_);

    Serial.print("Wi-Fi connection failed; retrying in ");
    Serial.print(wifiRetryDelay_ / 1000);
    Serial.println(" seconds");
}

void ConnectivityManager::maintainTimeSynchronization(
    ConnectivityEvents &events) {
    if (!wifiConnected_ || timeSynchronized_) {
        return;
    }

    uint32_t now = operations_.currentTime();
    if (now - lastNtpAttempt_ < ntpRetryIntervalMs_) {
        return;
    }

    Serial.println("Retrying NTP synchronization...");
    operations_.synchronizeTime();
    lastNtpAttempt_ = operations_.currentTime();
    timeSynchronized_ = operations_.isTimeSynchronized();

    if (timeSynchronized_) {
        events.timeSynchronized = true;
        Serial.println("System time synchronized");
    } else {
        Serial.println("NTP sync failed; retrying in 60 seconds");
    }
}