#include "ButtonDebouncer.h"

ButtonDebouncer::ButtonDebouncer(uint32_t debounceIntervalMs)
    : debounceIntervalMs_(debounceIntervalMs),
      lastRawChange_(0),
      rawPressed_(false),
      stablePressed_(false),
      initialized_(false) {
}

void ButtonDebouncer::begin(bool rawPressed, uint32_t now) {
    rawPressed_ = rawPressed;
    stablePressed_ = rawPressed;
    lastRawChange_ = now;
    initialized_ = true;
}

bool ButtonDebouncer::update(bool rawPressed, uint32_t now) {
    if (!initialized_) {
        begin(rawPressed, now);
        return false;
    }

    if (rawPressed != rawPressed_) {
        rawPressed_ = rawPressed;
        lastRawChange_ = now;
    }

    if (rawPressed_ == stablePressed_ ||
        now - lastRawChange_ < debounceIntervalMs_) {
        return false;
    }

    stablePressed_ = rawPressed_;
    return stablePressed_;
}