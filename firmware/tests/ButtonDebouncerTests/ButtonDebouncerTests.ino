#include <Arduino.h>

#include "AppConfig.h"
#include "ButtonDebouncer.h"

int failureCount = 0;

void expect(bool condition, const char *name) {
    Serial.print(condition ? "PASS: " : "FAIL: ");
    Serial.println(name);
    if (!condition) {
        ++failureCount;
    }
}

void testFirstSampleInitializesWithoutPress() {
    ButtonDebouncer button(AppConfig::BUTTON_DEBOUNCE_INTERVAL_MS);

    expect(
        !button.update(true, 100),
        "first pressed sample initializes without an event");
    expect(
        !button.update(true, 1000),
        "button held from startup does not produce an event");
}

void testBounceProducesOnePress() {
    ButtonDebouncer button(AppConfig::BUTTON_DEBOUNCE_INTERVAL_MS);
    button.begin(false, 0);

    expect(!button.update(true, 10), "initial press edge waits for debounce");
    expect(!button.update(false, 25), "release bounce produces no event");
    expect(!button.update(true, 40), "second press edge restarts debounce");
    expect(!button.update(true, 89), "press before 50 ms is ignored");
    expect(button.update(true, 90), "press at 50 ms produces one event");
    expect(!button.update(true, 1000), "holding the button produces no repeat");
}

void testStableReleaseAllowsAnotherPress() {
    ButtonDebouncer button(AppConfig::BUTTON_DEBOUNCE_INTERVAL_MS);
    button.begin(false, 0);

    button.update(true, 10);
    expect(button.update(true, 60), "first stable press is reported");
    expect(!button.update(false, 70), "release edge produces no event");
    expect(!button.update(false, 120), "stable release produces no event");
    expect(!button.update(true, 130), "second press waits for debounce");
    expect(button.update(true, 180), "second stable press is reported");
}

void testShortPressIsIgnored() {
    ButtonDebouncer button(AppConfig::BUTTON_DEBOUNCE_INTERVAL_MS);
    button.begin(false, 0);

    expect(!button.update(true, 10), "short press starts debounce");
    expect(!button.update(false, 30), "short release produces no event");
    expect(!button.update(false, 100), "short press remains ignored");
}

void testStartupPressMustBeReleasedBeforeNextEvent() {
    ButtonDebouncer button(AppConfig::BUTTON_DEBOUNCE_INTERVAL_MS);
    button.begin(true, 0);

    expect(!button.update(true, 100), "startup press is not replayed");
    expect(!button.update(false, 110), "startup release waits for debounce");
    expect(!button.update(false, 160), "startup release produces no event");
    expect(!button.update(true, 170), "post-startup press waits for debounce");
    expect(button.update(true, 220), "post-startup stable press is reported");
}

void testMillisWraparound() {
    ButtonDebouncer button(AppConfig::BUTTON_DEBOUNCE_INTERVAL_MS);
    button.begin(false, 0xFFFFFFE0UL);

    expect(
        !button.update(true, 0xFFFFFFF0UL),
        "press before millis wrap starts debounce");
    expect(
        !button.update(true, 0x00000021UL),
        "wrapped elapsed time below 50 ms is ignored");
    expect(
        button.update(true, 0x00000022UL),
        "wrapped elapsed time at 50 ms produces an event");
}

void setup() {
    Serial.begin(115200);
    while (!Serial);
    delay(3000);

    Serial.println("TEST_SUITE: ButtonDebouncerTests");
    expect(
        AppConfig::BUTTON_DEBOUNCE_INTERVAL_MS == 50UL,
        "button debounce interval is 50 ms");
    testFirstSampleInitializesWithoutPress();
    testBounceProducesOnePress();
    testStableReleaseAllowsAnotherPress();
    testShortPressIsIgnored();
    testStartupPressMustBeReleasedBeforeNextEvent();
    testMillisWraparound();
}

void loop() {
    Serial.print("TEST_RESULT: ");
    Serial.println(failureCount == 0 ? "PASS" : "FAIL");
    delay(1000);
}