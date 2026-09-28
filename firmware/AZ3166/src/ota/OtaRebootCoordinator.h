#ifndef OTA_REBOOT_COORDINATOR_H
#define OTA_REBOOT_COORDINATOR_H

#include <stdint.h>

class OtaRebootCoordinator {
public:
    explicit OtaRebootCoordinator(uint32_t delayMs)
        : delayMs_(delayMs), deadline_(0), pending_(false) {
    }

    void schedule(uint32_t now) {
        if (!pending_) {
            deadline_ = now + delayMs_;
            pending_ = true;
        }
    }

    bool pending() const {
        return pending_;
    }

    bool due(uint32_t now) const {
        return pending_ &&
            static_cast<int32_t>(now - deadline_) >= 0;
    }

    bool cloudAllowed() const {
        return !pending_;
    }

private:
    uint32_t delayMs_;
    uint32_t deadline_;
    bool pending_;
};

#endif
