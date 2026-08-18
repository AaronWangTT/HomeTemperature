#ifndef BUTTON_CONTROLLER_H
#define BUTTON_CONTROLLER_H

#include <Arduino.h>

#include "ButtonDebouncer.h"

struct ButtonEvents {
    bool uploadRequested;
    bool toggleUploadPause;
};

class ButtonController {
public:
    ButtonController(
        PinName uploadButtonPin,
        PinName pauseButtonPin,
        uint32_t debounceIntervalMs);

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
};

#endif