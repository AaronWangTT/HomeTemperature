#include "WatchdogController.h"

#include <Arduino.h>

WatchdogController::WatchdogController(float timeoutMs)
    : timeoutMs_(timeoutMs),
      enabled_(false) {
}

void WatchdogController::begin() {
    bool watchdogCausedReset = watchdog_.resetTriggered();
    enabled_ = watchdog_.configure(timeoutMs_);

    if (watchdogCausedReset) {
        Serial.println("Restart reason: watchdog timeout");
    }

    if (enabled_) {
        Serial.print("Watchdog enabled: ");
        Serial.print(static_cast<unsigned long>(timeoutMs_));
        Serial.println(" ms timeout");
    } else {
        Serial.println("Failed to enable watchdog");
    }
}

void WatchdogController::reset() {
    if (enabled_) {
        watchdog_.resetTimer();
    }
}