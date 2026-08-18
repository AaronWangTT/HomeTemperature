#include <Arduino.h>

#include "AppConfig.h"
#include "ButtonController.h"

int failureCount = 0;

void expect(bool condition, const char *name) {
    Serial.print(condition ? "PASS: " : "FAIL: ");
    Serial.println(name);
    if (!condition) {
        ++failureCount;
    }
}

void expectNoEvents(const ButtonEvents &events, const char *name) {
    expect(
        !events.uploadRequested && !events.toggleUploadPause,
        name);
}

void testUploadButtonEvent() {
    ButtonController buttons(
        USER_BUTTON_A,
        USER_BUTTON_B,
        AppConfig::BUTTON_DEBOUNCE_INTERVAL_MS);

    expectNoEvents(
        buttons.updateFromInputs(false, false, 0),
        "initial released state produces no events");
    expectNoEvents(
        buttons.updateFromInputs(true, false, 10),
        "button A waits for debounce");

    ButtonEvents events = buttons.updateFromInputs(true, false, 60);
    expect(events.uploadRequested, "button A requests an upload");
    expect(
        !events.toggleUploadPause,
        "button A does not toggle upload pause");
    expectNoEvents(
        buttons.updateFromInputs(true, false, 1000),
        "holding button A produces no repeated events");
}

void testPauseButtonEvent() {
    ButtonController buttons(
        USER_BUTTON_A,
        USER_BUTTON_B,
        AppConfig::BUTTON_DEBOUNCE_INTERVAL_MS);

    buttons.updateFromInputs(false, false, 0);
    expectNoEvents(
        buttons.updateFromInputs(false, true, 10),
        "button B waits for debounce");

    ButtonEvents events = buttons.updateFromInputs(false, true, 60);
    expect(
        !events.uploadRequested,
        "button B does not request an upload");
    expect(events.toggleUploadPause, "button B toggles upload pause");
    expectNoEvents(
        buttons.updateFromInputs(false, true, 1000),
        "holding button B produces no repeated events");
}

void testSimultaneousButtonEvents() {
    ButtonController buttons(
        USER_BUTTON_A,
        USER_BUTTON_B,
        AppConfig::BUTTON_DEBOUNCE_INTERVAL_MS);

    buttons.updateFromInputs(false, false, 0);
    buttons.updateFromInputs(true, true, 10);
    ButtonEvents events = buttons.updateFromInputs(true, true, 60);

    expect(
        events.uploadRequested && events.toggleUploadPause,
        "simultaneous stable presses produce both events");
}

void testStartupPressIsNotReplayed() {
    ButtonController buttons(
        USER_BUTTON_A,
        USER_BUTTON_B,
        AppConfig::BUTTON_DEBOUNCE_INTERVAL_MS);

    expectNoEvents(
        buttons.updateFromInputs(true, false, 0),
        "button A held at startup produces no event");
    expectNoEvents(
        buttons.updateFromInputs(true, false, 100),
        "startup button A hold remains quiet");
    expectNoEvents(
        buttons.updateFromInputs(false, false, 110),
        "startup button A release waits for debounce");
    expectNoEvents(
        buttons.updateFromInputs(false, false, 160),
        "startup button A release produces no event");
    expectNoEvents(
        buttons.updateFromInputs(true, false, 170),
        "new button A press waits for debounce");

    ButtonEvents events = buttons.updateFromInputs(true, false, 220);
    expect(
        events.uploadRequested && !events.toggleUploadPause,
        "new button A press after release is reported");
}

void setup() {
    Serial.begin(115200);
    while (!Serial);
    delay(3000);

    Serial.println("TEST_SUITE: ButtonControllerTests");
    testUploadButtonEvent();
    testPauseButtonEvent();
    testSimultaneousButtonEvents();
    testStartupPressIsNotReplayed();
}

void loop() {
    Serial.print("TEST_RESULT: ");
    Serial.println(failureCount == 0 ? "PASS" : "FAIL");
    delay(1000);
}