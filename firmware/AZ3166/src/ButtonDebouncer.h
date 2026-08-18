#ifndef BUTTON_DEBOUNCER_H
#define BUTTON_DEBOUNCER_H

#include <stdint.h>

class ButtonDebouncer {
public:
    explicit ButtonDebouncer(uint32_t debounceIntervalMs);

    void begin(bool rawPressed, uint32_t now);

    // Returns true once when a new press remains stable for the interval.
    bool update(bool rawPressed, uint32_t now);

private:
    uint32_t debounceIntervalMs_;
    uint32_t lastRawChange_;
    bool rawPressed_;
    bool stablePressed_;
    bool initialized_;
};

#endif