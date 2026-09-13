#ifndef AZ3166_LOCAL_DISCOVERY_OPERATIONS_H
#define AZ3166_LOCAL_DISCOVERY_OPERATIONS_H

#include <ArduinoMDNS.h>
#include "LocalDiscovery.h"
#include "MdnsUdpTransport.h"
#include "rtos.h"

class Az3166LocalDiscoveryOperations : public LocalDiscoveryOperations {
public:
    Az3166LocalDiscoveryOperations();
    ~Az3166LocalDiscoveryOperations() override;

    uint32_t currentTime() override;
    bool start(uint32_t address, const LocalDiscoveryService &service) override;
    void stop() override;
    bool isHealthy() override;

protected:
    virtual osStatus startWorker();
    virtual bool configureResponder(uint32_t address, const LocalDiscoveryService &service);
    virtual void endResponder();
    virtual bool serviceResponder();
    bool serviceOnce();

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

#endif