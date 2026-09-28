#ifndef BUTTON_CONTROLLER_H
#define BUTTON_CONTROLLER_H

#include <Arduino.h>

#include "ButtonDebouncer.h"

struct ButtonEvents {
    bool uploadRequested;
    bool toggleUploadPause;
    bool otaRequested;
};

class ButtonController {
public:
    ButtonController(
        PinName uploadButtonPin,
        PinName pauseButtonPin,
        uint32_t debounceIntervalMs,
        uint32_t otaHoldIntervalMs = 2000UL);

    void begin();
    ButtonEvents update();

    ButtonEvents updateFromInputs(
        bool uploadButtonPressed,
        bool pauseButtonPressed,
        uint32_t now);

private:
    bool isPressed(PinName pin) const;

    PinName uploadButtonPin_;
    PinName pauseButtonPin_;
    ButtonDebouncer uploadButton_;
    ButtonDebouncer pauseButton_;
    uint32_t otaHoldIntervalMs_;
    uint32_t chordStarted_;
    bool chordActive_;
    bool chordReported_;
    bool chordEligible_;
    bool pendingUpload_;
    bool pendingPause_;
};

#endif