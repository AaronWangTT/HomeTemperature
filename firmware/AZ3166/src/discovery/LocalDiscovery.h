#ifndef LOCAL_DISCOVERY_H
#define LOCAL_DISCOVERY_H

#include <stdint.h>

struct LocalDiscoveryService {
    const char *hostname;
    const char *serviceName;
    uint16_t port;
    const char *txtRecord;
};

struct LocalDiscoveryOperations {
    uint32_t (*currentTime)();
    bool (*start)(uint32_t address, const LocalDiscoveryService &service);
    void (*stop)();
    bool (*isHealthy)();
};

class LocalDiscovery {
public:
    LocalDiscovery(uint32_t retryIntervalMs,
                   const LocalDiscoveryService &service);
    LocalDiscovery(uint32_t retryIntervalMs,
                   const LocalDiscoveryService &service,
                   const LocalDiscoveryOperations &operations);
    void update(bool serviceAvailable, uint32_t address);
    bool isRunning() const;

private:
    static LocalDiscoveryOperations defaultOperations();

    uint32_t retryIntervalMs_;
    LocalDiscoveryService service_;
    LocalDiscoveryOperations operations_;
    uint32_t requestedAddress_;
    uint32_t lastAttempt_;
    bool attempted_;
    bool running_;
};

#endif