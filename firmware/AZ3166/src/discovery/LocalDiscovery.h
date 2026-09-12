#ifndef LOCAL_DISCOVERY_H
#define LOCAL_DISCOVERY_H

#include <atomic>
#include <stdint.h>

struct LocalDiscoveryService {
    const char *hostname;
    const char *serviceName;
    uint16_t port;
    const char *txtRecord;
};

class LocalDiscoveryOperations {
public:
    virtual ~LocalDiscoveryOperations() = default;

    virtual uint32_t currentTime() = 0;
    virtual bool start(uint32_t address, const LocalDiscoveryService &service) = 0;
    virtual void stop() = 0;
    virtual bool isHealthy() = 0;

private:
    friend class LocalDiscovery;
    bool tryAcquire();
    void release();

    std::atomic_flag sessionInUse_ = ATOMIC_FLAG_INIT;
};

class LocalDiscovery {
public:
    LocalDiscovery(uint32_t retryIntervalMs,
                   const LocalDiscoveryService &service);
    LocalDiscovery(uint32_t retryIntervalMs,
                   const LocalDiscoveryService &service,
                   LocalDiscoveryOperations &operations);
    ~LocalDiscovery();

    LocalDiscovery(const LocalDiscovery &) = delete;
    LocalDiscovery &operator=(const LocalDiscovery &) = delete;
    LocalDiscovery(LocalDiscovery &&) = delete;
    LocalDiscovery &operator=(LocalDiscovery &&) = delete;

    void update(bool serviceAvailable, uint32_t address);
    bool isRunning() const;

private:
    static LocalDiscoveryOperations &defaultOperations();
    void stopSession();

    uint32_t retryIntervalMs_;
    LocalDiscoveryService service_;
    LocalDiscoveryOperations &operations_;
    uint32_t requestedAddress_;
    uint32_t lastAttempt_;
    bool attempted_;
    bool running_;
    bool sessionOwned_;
};

#endif