#include "LocalDiscovery.h"

#include <mutex>
#include <Arduino.h>
#include "MdnsUdpTransport.h"
#include "mdns/MDNS.h"
#include "rtos.h"

namespace {

class Az3166LocalDiscoveryOperations : public LocalDiscoveryOperations {
public:
    Az3166LocalDiscoveryOperations();
    ~Az3166LocalDiscoveryOperations() override;

    uint32_t currentTime() override;
    bool start(uint32_t address, const LocalDiscoveryService &service) override;
    void stop() override;
    bool isHealthy() override;

private:
    void serviceDiscovery();

    MdnsUdpTransport transport_;
    MDNS responder_;
    rtos::Mutex responderMutex_;
    alignas(8) unsigned char workerStack_[4096];
    rtos::Thread worker_;
    bool workerStarted_;
    bool shutdown_;
    bool responderRunning_;
    bool followupPending_;
    uint32_t firstAnnouncement_;
};

Az3166LocalDiscoveryOperations::Az3166LocalDiscoveryOperations()
    : responder_(transport_),
      worker_(osPriorityNormal, sizeof(workerStack_), workerStack_),
      workerStarted_(false),
      shutdown_(false),
      responderRunning_(false),
      followupPending_(false),
      firstAnnouncement_(0) {
}

Az3166LocalDiscoveryOperations::~Az3166LocalDiscoveryOperations() {
    bool started;
    {
        std::lock_guard<rtos::Mutex> lock(responderMutex_);
        shutdown_ = true;
        started = workerStarted_;
    }
    if (started) {
        worker_.join();
    }
    stop();
}

void Az3166LocalDiscoveryOperations::serviceDiscovery() {
    for (;;) {
        {
            std::lock_guard<rtos::Mutex> lock(responderMutex_);
            if (shutdown_) {
                return;
            }
            if (responderRunning_) {
                responder_.run();
                if (followupPending_ && millis() - firstAnnouncement_ >= 1000UL) {
                    responder_.announce();
                    followupPending_ = false;
                }
                responderRunning_ = !transport_.failed();
            }
        }
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
    std::lock_guard<rtos::Mutex> lock(responderMutex_);
    transport_.setLocalIPv4Address(address);
    IPAddress localAddress(
        static_cast<uint8_t>(address >> 24),
        static_cast<uint8_t>(address >> 16),
        static_cast<uint8_t>(address >> 8),
        static_cast<uint8_t>(address));
    bool started = responder_.begin(localAddress, service.hostname) &&
        responder_.addServiceRecord(
            service.serviceName, service.port, MDNSServiceTCP, service.txtRecord);
    if (started && !workerStarted_) {
        workerStarted_ = worker_.start(mbed::callback(
            this, &Az3166LocalDiscoveryOperations::serviceDiscovery)) == osOK;
        started = workerStarted_;
    }
    responderRunning_ = started;
    followupPending_ = started;
    firstAnnouncement_ = millis();
    if (!started) {
        responder_.end();
    }
    return started;
}

void Az3166LocalDiscoveryOperations::stop() {
    std::lock_guard<rtos::Mutex> lock(responderMutex_);
    responderRunning_ = false;
    followupPending_ = false;
    responder_.end();
}

bool Az3166LocalDiscoveryOperations::isHealthy() {
    std::lock_guard<rtos::Mutex> lock(responderMutex_);
    return responderRunning_;
}

}

bool LocalDiscoveryOperations::tryAcquire() {
    return !sessionInUse_.test_and_set(std::memory_order_acquire);
}

void LocalDiscoveryOperations::release() {
    sessionInUse_.clear(std::memory_order_release);
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
    running_(false),
    sessionOwned_(false) {
}

LocalDiscovery::~LocalDiscovery() {
    stopSession();
}

LocalDiscoveryOperations &LocalDiscovery::defaultOperations() {
    static Az3166LocalDiscoveryOperations operations;
    return operations;
}

void LocalDiscovery::stopSession() {
    if (sessionOwned_) {
        operations_.stop();
        sessionOwned_ = false;
        operations_.release();
    }
    running_ = false;
}

void LocalDiscovery::update(bool serviceAvailable, uint32_t address) {
    uint32_t requested = serviceAvailable ? address : 0;
    if (requested != requestedAddress_) {
        stopSession();
        attempted_ = false;
        requestedAddress_ = requested;
    }
    if (requestedAddress_ == 0) {
        return;
    }

    if (running_) {
        if (!operations_.isHealthy()) {
            stopSession();
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

    sessionOwned_ = operations_.tryAcquire();
    if (!sessionOwned_) {
        attempted_ = true;
        lastAttempt_ = now;
        return;
    }

    running_ = operations_.start(requestedAddress_, service_);
    attempted_ = true;
    lastAttempt_ = operations_.currentTime();
    if (!running_) {
        stopSession();
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