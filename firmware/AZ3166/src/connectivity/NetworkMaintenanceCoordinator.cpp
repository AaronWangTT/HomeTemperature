#include "NetworkMaintenanceCoordinator.h"

#include <mutex>

NetworkMaintenanceCoordinator::NetworkMaintenanceCoordinator()
    : cloudInFlight_(false),
      otaReserved_(false),
      otaLeased_(false),
      otaGeneration_(0) {
}

bool NetworkMaintenanceCoordinator::tryBeginCloud() {
    std::lock_guard<rtos::Mutex> lock(mutex_);
    if (cloudInFlight_ || otaReserved_) {
        return false;
    }
    cloudInFlight_ = true;
    return true;
}

void NetworkMaintenanceCoordinator::endCloud() {
    std::lock_guard<rtos::Mutex> lock(mutex_);
    cloudInFlight_ = false;
}

bool NetworkMaintenanceCoordinator::reserveOta(uint32_t generation) {
    std::lock_guard<rtos::Mutex> lock(mutex_);
    if (otaReserved_ || generation == 0) {
        return false;
    }
    otaReserved_ = true;
    otaLeased_ = false;
    otaGeneration_ = generation;
    return true;
}

bool NetworkMaintenanceCoordinator::tryGrantOta(uint32_t generation) {
    std::lock_guard<rtos::Mutex> lock(mutex_);
    if (!otaReserved_ || otaGeneration_ != generation || cloudInFlight_) {
        return false;
    }
    otaLeased_ = true;
    return true;
}

void NetworkMaintenanceCoordinator::releaseOta(uint32_t generation) {
    std::lock_guard<rtos::Mutex> lock(mutex_);
    if (otaReserved_ && otaGeneration_ == generation) {
        otaReserved_ = false;
        otaLeased_ = false;
        otaGeneration_ = 0;
    }
}

bool NetworkMaintenanceCoordinator::otaBusy() const {
    std::lock_guard<rtos::Mutex> lock(mutex_);
    return otaReserved_;
}
