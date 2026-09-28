#ifndef NETWORK_MAINTENANCE_COORDINATOR_H
#define NETWORK_MAINTENANCE_COORDINATOR_H

#include <stdint.h>
#include "rtos.h"

class NetworkMaintenanceCoordinator {
public:
    NetworkMaintenanceCoordinator();

    bool tryBeginCloud();
    void endCloud();
    bool reserveOta(uint32_t generation);
    bool tryGrantOta(uint32_t generation);
    void releaseOta(uint32_t generation);
    bool otaBusy() const;

private:
    mutable rtos::Mutex mutex_;
    bool cloudInFlight_;
    bool otaReserved_;
    bool otaLeased_;
    uint32_t otaGeneration_;
};

#endif
