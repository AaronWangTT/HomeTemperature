#include "LocalDiscovery.h"

#include <Arduino.h>
#include "AppConfig.h"
#include "MdnsUdpTransport.h"
#include "mdns/MDNS.h"
#include "rtos.h"

namespace {

MdnsUdpTransport transport;
MDNS responder(transport);
rtos::Mutex responderMutex;
alignas(8) unsigned char workerStack[4096];
rtos::Thread worker(osPriorityNormal, sizeof(workerStack), workerStack);
bool workerStarted = false;
bool responderRunning = false;
bool followupPending = false;
uint32_t firstAnnouncement = 0;

void serviceDiscovery() {
    for (;;) {
        responderMutex.lock();
        if (responderRunning) {
            responder.run();
            if (followupPending && millis() - firstAnnouncement >= 1000UL) {
                responder.announce();
                followupPending = false;
            }
            responderRunning = !transport.failed();
        }
        responderMutex.unlock();
        rtos::Thread::wait(20);
    }
}

uint32_t platformCurrentTime() {
    return millis();
}

bool platformStart(uint32_t address) {
    responderMutex.lock();
    transport.setLocalIPv4Address(address);
    IPAddress localAddress(
        static_cast<uint8_t>(address >> 24),
        static_cast<uint8_t>(address >> 16),
        static_cast<uint8_t>(address >> 8),
        static_cast<uint8_t>(address));
    bool started = responder.begin(localAddress, AppConfig::LOCAL_HOSTNAME) &&
        responder.addServiceRecord(
            "az3166._http", AppConfig::LOCAL_TELEMETRY_PORT,
            MDNSServiceTCP, "\x13" "path=/api/telemetry");
    if (started && !workerStarted) {
        workerStarted = worker.start(mbed::callback(serviceDiscovery)) == osOK;
        started = workerStarted;
    }
    responderRunning = started;
    followupPending = started;
    firstAnnouncement = millis();
    if (!started) {
        responder.end();
    }
    responderMutex.unlock();
    return started;
}

void platformStop() {
    responderMutex.lock();
    responderRunning = false;
    followupPending = false;
    responder.end();
    responderMutex.unlock();
}

bool platformIsHealthy() {
    responderMutex.lock();
    bool healthy = responderRunning;
    responderMutex.unlock();
    return healthy;
}

}

LocalDiscovery::LocalDiscovery(uint32_t retryIntervalMs)
    : LocalDiscovery(retryIntervalMs, defaultOperations()) {
}

LocalDiscovery::LocalDiscovery(
    uint32_t retryIntervalMs,
    const LocalDiscoveryOperations &operations)
    : retryIntervalMs_(retryIntervalMs),
      operations_(operations),
      requestedAddress_(0),
      lastAttempt_(0),
      attempted_(false),
      running_(false) {
}

LocalDiscoveryOperations LocalDiscovery::defaultOperations() {
    LocalDiscoveryOperations operations = {
        platformCurrentTime, platformStart, platformStop, platformIsHealthy
    };
    return operations;
}

void LocalDiscovery::update(bool wifiConnected, uint32_t address) {
    uint32_t requested = wifiConnected ? address : 0;
    if (requested != requestedAddress_) {
        if (running_) {
            operations_.stop();
        }
        running_ = false;
        attempted_ = false;
        requestedAddress_ = requested;
    }
    if (requestedAddress_ == 0) {
        return;
    }

    if (running_) {
        if (!operations_.isHealthy()) {
            operations_.stop();
            running_ = false;
            attempted_ = true;
            lastAttempt_ = operations_.currentTime();
            Serial.println("mDNS transport failed; retrying later");
        }
        return;
    }

    uint32_t now = operations_.currentTime();
    if (attempted_ && now - lastAttempt_ < retryIntervalMs_) {
        return;
    }

    running_ = operations_.start(requestedAddress_);
    attempted_ = true;
    lastAttempt_ = operations_.currentTime();
    if (!running_) {
        operations_.stop();
        Serial.println("mDNS start failed; retrying later");
    } else {
        Serial.print("Local mDNS endpoint: http://");
        Serial.print(AppConfig::LOCAL_HOSTNAME);
        Serial.println(".local/api/telemetry");
    }
}

bool LocalDiscovery::isRunning() const {
    return running_;
}