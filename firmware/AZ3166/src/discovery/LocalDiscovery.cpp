#include "LocalDiscovery.h"

#include <Arduino.h>
#include "MdnsUdpTransport.h"
#include "mdns/MDNS.h"
#include "rtos.h"

namespace {

class Az3166LocalDiscoveryOperations : public LocalDiscoveryOperations {
public:
    uint32_t currentTime() override;
    bool start(uint32_t address, const LocalDiscoveryService &service) override;
    void stop() override;
    bool isHealthy() override;
};

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

uint32_t Az3166LocalDiscoveryOperations::currentTime() {
    return millis();
}

bool Az3166LocalDiscoveryOperations::start(
    uint32_t address, const LocalDiscoveryService &service) {
    if (service.hostname == NULL || service.hostname[0] == '\0' ||
        service.serviceName == NULL || service.serviceName[0] == '\0' ||
        service.port == 0) {
        return false;
    }
    responderMutex.lock();
    transport.setLocalIPv4Address(address);
    IPAddress localAddress(
        static_cast<uint8_t>(address >> 24),
        static_cast<uint8_t>(address >> 16),
        static_cast<uint8_t>(address >> 8),
        static_cast<uint8_t>(address));
    bool started = responder.begin(localAddress, service.hostname) &&
        responder.addServiceRecord(
            service.serviceName, service.port, MDNSServiceTCP, service.txtRecord);
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

void Az3166LocalDiscoveryOperations::stop() {
    responderMutex.lock();
    responderRunning = false;
    followupPending = false;
    responder.end();
    responderMutex.unlock();
}

bool Az3166LocalDiscoveryOperations::isHealthy() {
    responderMutex.lock();
    bool healthy = responderRunning;
    responderMutex.unlock();
    return healthy;
}

}

LocalDiscovery::LocalDiscovery(
    uint32_t retryIntervalMs,
    const LocalDiscoveryService &service)
    : LocalDiscovery(retryIntervalMs, service, defaultOperations()) {
}

LocalDiscovery::LocalDiscovery(
    uint32_t retryIntervalMs,
        const LocalDiscoveryService &service,
    LocalDiscoveryOperations &operations)
    : retryIntervalMs_(retryIntervalMs),
            service_(service),
      operations_(operations),
      requestedAddress_(0),
      lastAttempt_(0),
      attempted_(false),
      running_(false) {
}

LocalDiscoveryOperations &LocalDiscovery::defaultOperations() {
    static Az3166LocalDiscoveryOperations operations;
    return operations;
}

void LocalDiscovery::update(bool serviceAvailable, uint32_t address) {
    uint32_t requested = serviceAvailable ? address : 0;
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

    running_ = operations_.start(requestedAddress_, service_);
    attempted_ = true;
    lastAttempt_ = operations_.currentTime();
    if (!running_) {
        operations_.stop();
        Serial.println("mDNS start failed; retrying later");
    } else {
        Serial.print("Local mDNS service: ");
        Serial.print(service_.hostname);
        Serial.print(".local:");
        Serial.println(service_.port);
    }
}

bool LocalDiscovery::isRunning() const {
    return running_;
}