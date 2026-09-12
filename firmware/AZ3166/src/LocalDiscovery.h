#ifndef LOCAL_DISCOVERY_H
#define LOCAL_DISCOVERY_H

#include <stdint.h>

struct LocalDiscoveryOperations {
    uint32_t (*currentTime)();
    bool (*start)(uint32_t address);
    void (*stop)();
    bool (*isHealthy)();
};

class LocalDiscovery {
public:
    explicit LocalDiscovery(uint32_t retryIntervalMs);
    LocalDiscovery(uint32_t retryIntervalMs,
                   const LocalDiscoveryOperations &operations);
    void update(bool wifiConnected, uint32_t address);
    bool isRunning() const;

private:
    static LocalDiscoveryOperations defaultOperations();

    uint32_t retryIntervalMs_;
    LocalDiscoveryOperations operations_;
    uint32_t requestedAddress_;
    uint32_t lastAttempt_;
    bool attempted_;
    bool running_;
};

#endif