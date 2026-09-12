#ifndef CONNECTIVITY_MANAGER_H
#define CONNECTIVITY_MANAGER_H

#include <stdint.h>

struct ConnectivityEvents {
    bool wifiConnected;
    bool wifiDisconnected;
    bool timeSynchronized;
    bool localAddressChanged;
};

class ConnectivityOperations {
public:
    virtual ~ConnectivityOperations() = default;

    virtual uint32_t currentTime() = 0;
    virtual bool isWiFiConnected() = 0;
    virtual void disconnectWiFi() = 0;
    virtual bool connectWiFi() = 0;
    virtual bool isTimeSynchronized() = 0;
    virtual void synchronizeTime() = 0;
    virtual uint32_t readLocalIPv4Address() = 0;
};

class ConnectivityManager {
public:
    ConnectivityManager(
        uint32_t wifiStatusIntervalMs,
        uint32_t wifiRetryInitialMs,
        uint32_t wifiRetryMaxMs,
        uint32_t ntpRetryIntervalMs);

    ConnectivityManager(
        uint32_t wifiStatusIntervalMs,
        uint32_t wifiRetryInitialMs,
        uint32_t wifiRetryMaxMs,
        uint32_t ntpRetryIntervalMs,
        ConnectivityOperations &operations);

    ConnectivityEvents update();
    bool isWiFiConnected() const;
    bool isTimeSynchronized() const;
    uint32_t localIPv4Address() const;
    void printLocalHttpEndpoint(const char *path) const;
    void printTimeSynchronizationStatus() const;

    static uint32_t parseLocalIPv4Address(const char *addressText);
    static uint32_t nextRetryDelay(
        uint32_t currentDelay,
        uint32_t initialDelay,
        uint32_t maximumDelay);

private:
    static ConnectivityOperations &defaultOperations();

    void maintainWiFi(ConnectivityEvents &events);
    void attemptWiFiConnection(ConnectivityEvents &events);
    void handleWiFiConnected(ConnectivityEvents &events);
    void setLocalIPv4Address(uint32_t address, ConnectivityEvents &events);
    void printConnectionDetails() const;
    void scheduleWiFiRetry();
    void maintainTimeSynchronization(ConnectivityEvents &events);

    uint32_t wifiStatusIntervalMs_;
    uint32_t wifiRetryInitialMs_;
    uint32_t wifiRetryMaxMs_;
    uint32_t ntpRetryIntervalMs_;
    ConnectivityOperations &operations_;
    bool wifiConnected_;
    bool timeSynchronized_;
    uint32_t localIPv4Address_;
    uint32_t lastWiFiStatusCheck_;
    uint32_t lastWiFiAttempt_;
    uint32_t wifiRetryDelay_;
    uint32_t lastNtpAttempt_;
};

#endif