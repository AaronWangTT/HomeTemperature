#include "ButtonController.h"

ButtonController::ButtonController(
    PinName uploadButtonPin,
    PinName pauseButtonPin,
    uint32_t debounceIntervalMs,
    uint32_t otaHoldIntervalMs)
    : uploadButtonPin_(uploadButtonPin),
      pauseButtonPin_(pauseButtonPin),
      uploadButton_(debounceIntervalMs),
      pauseButton_(debounceIntervalMs),
      otaHoldIntervalMs_(otaHoldIntervalMs),
      chordStarted_(0),
      chordActive_(false),
      chordReported_(false),
      chordEligible_(false),
      pendingUpload_(false),
      pendingPause_(false) {
}

void ButtonController::begin() {
    uint32_t now = millis();
    pinMode(uploadButtonPin_, INPUT);
    pinMode(pauseButtonPin_, INPUT);

    uploadButton_.begin(isPressed(uploadButtonPin_), now);
    pauseButton_.begin(isPressed(pauseButtonPin_), now);

    chordEligible_ = !isPressed(uploadButtonPin_) || !isPressed(pauseButtonPin_);
    Serial.println("A: upload; B: pause; hold A+B: local OTA");
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
        pauseButton_.update(pauseButtonPressed, now),
        false
    };
    if (!uploadButtonPressed || !pauseButtonPressed) {
        if (chordActive_ && !chordReported_) {
            events.uploadRequested = events.uploadRequested || pendingUpload_;
            events.toggleUploadPause =
                events.toggleUploadPause || pendingPause_;
        }
        chordActive_ = false;
        chordReported_ = false;
        pendingUpload_ = false;
        pendingPause_ = false;
        chordEligible_ = true;
        return events;
    }
    if (!chordEligible_) {
        events.uploadRequested = false;
        events.toggleUploadPause = false;
        return events;
    }
    if (!chordActive_) {
        chordActive_ = true;
        chordStarted_ = now;
    }
    pendingUpload_ = pendingUpload_ || events.uploadRequested;
    pendingPause_ = pendingPause_ || events.toggleUploadPause;
    events.uploadRequested = false;
    events.toggleUploadPause = false;
    if (!chordReported_ && now - chordStarted_ >= otaHoldIntervalMs_) {
        chordReported_ = true;
        pendingUpload_ = false;
        pendingPause_ = false;
        events.otaRequested = true;
    }
    return events;
}

bool ButtonController::isPressed(PinName pin) const {
    // AZ3166 buttons are active-low.
    return digitalRead(pin) == LOW;
}