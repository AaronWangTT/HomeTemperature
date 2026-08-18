#ifndef WATCHDOG_CONTROLLER_H
#define WATCHDOG_CONTROLLER_H

#include "Watchdog.h"

class WatchdogController {
public:
    explicit WatchdogController(float timeoutMs);

    void begin();
    void reset();

private:
    Watchdog watchdog_;
    const float timeoutMs_;
    bool enabled_;
};

#endif