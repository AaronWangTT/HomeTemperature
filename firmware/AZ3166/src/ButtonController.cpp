#include "ButtonController.h"

ButtonController::ButtonController(
    PinName uploadButtonPin,
    PinName pauseButtonPin,
    uint32_t debounceIntervalMs)
    : uploadButtonPin_(uploadButtonPin),
      pauseButtonPin_(pauseButtonPin),
      uploadButton_(debounceIntervalMs),
      pauseButton_(debounceIntervalMs) {
}

void ButtonController::begin() {
    uint32_t now = millis();
    pinMode(uploadButtonPin_, INPUT);
    pinMode(pauseButtonPin_, INPUT);

    uploadButton_.begin(isPressed(uploadButtonPin_), now);
    pauseButton_.begin(isPressed(pauseButtonPin_), now);

    Serial.println("Button A: upload now; Button B: pause/resume scheduled uploads");
}

ButtonEvents ButtonController::update() {
    uint32_t now = millis();
    return updateFromInputs(
        isPressed(uploadButtonPin_),
        isPressed(pauseButtonPin_),
        now);
}

ButtonEvents ButtonController::updateFromInputs(
    bool uploadButtonPressed,
    bool pauseButtonPressed,
    uint32_t now) {
    ButtonEvents events = {
        uploadButton_.update(uploadButtonPressed, now),
        pauseButton_.update(pauseButtonPressed, now)
    };
    return events;
}

bool ButtonController::isPressed(PinName pin) const {
    // AZ3166 buttons are active-low.
    return digitalRead(pin) == LOW;
}