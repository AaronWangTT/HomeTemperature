#include <Arduino.h>
#include <math.h>

#include "AppConfig.h"
#include "TelemetryService.h"

int failureCount = 0;

void expect(bool condition, const char *name) {
    Serial.print(condition ? "PASS: " : "FAIL: ");
    Serial.println(name);
    if (!condition) {
        ++failureCount;
    }
}

void testDtostrfRegression() {
    char formatted[16];

    dtostrf(45.0, 0, 1, formatted);
    expect(strcmp(formatted, "45.0") == 0,
           "dtostrf emits exactly one decimal digit for whole values");

    dtostrf(9.96, 0, 1, formatted);
    expect(strcmp(formatted, "10.0") == 0,
           "dtostrf carries rounding into the integer part");

    dtostrf(1.25, 7, 2, formatted);
    expect(strcmp(formatted, "   1.25") == 0,
           "positive dtostrf width right aligns the result");

    dtostrf(1.25, -7, 2, formatted);
    expect(strcmp(formatted, "1.25   ") == 0,
           "negative dtostrf width left aligns the result");
}

void testKnownPayload() {
    const char expected[] =
        "{\"deviceId\":\"az3166-test\",\"temperature\":23.5,"
        "\"humidity\":45.0,\"pressure\":1013.2}";
    TelemetryReading reading = {23.5f, 45.0f, 1013.2f};
    char payload[sizeof(expected)];

    int length = TelemetryService::formatPayload(
        "az3166-test",
        reading,
        payload,
        sizeof(payload));

    expect(length == static_cast<int>(strlen(expected)),
           "known payload reports its JSON length");
    expect(strcmp(payload, expected) == 0,
           "known reading produces the expected JSON");
}

void testSignedAndBoundaryValues() {
    const char expected[] =
        "{\"deviceId\":\"boundary\",\"temperature\":-5.0,"
        "\"humidity\":0.0,\"pressure\":999.9}";
    TelemetryReading reading = {-5.0f, 0.0f, 999.9f};
    char payload[sizeof(expected)];

    int length = TelemetryService::formatPayload(
        "boundary",
        reading,
        payload,
        sizeof(payload));

    expect(length == static_cast<int>(strlen(expected)),
           "signed payload reports its JSON length");
    expect(strcmp(payload, expected) == 0,
           "signed and zero values retain one decimal place");
}

void testRounding() {
    const char expected[] =
        "{\"deviceId\":\"rounding\",\"temperature\":-1.3,"
        "\"humidity\":45.0,\"pressure\":1013.3}";
    TelemetryReading reading = {-1.26f, 45.04f, 1013.26f};
    char payload[sizeof(expected)];

    int length = TelemetryService::formatPayload(
        "rounding",
        reading,
        payload,
        sizeof(payload));

    expect(length == static_cast<int>(strlen(expected)),
           "rounded payload reports its JSON length");
    expect(strcmp(payload, expected) == 0,
           "telemetry values round to one decimal place");
}

void testNonFiniteValues() {
    char payload[AppConfig::TELEMETRY_PAYLOAD_SIZE];
    TelemetryReading notANumber = {NAN, 45.0f, 1013.2f};
    expect(
        TelemetryService::formatPayload(
            "invalid",
            notANumber,
            payload,
            sizeof(payload)) == TelemetryService::PAYLOAD_FORMAT_ERROR,
        "NaN telemetry value is rejected");
    expect(payload[0] == '\0', "NaN payload buffer remains empty");

    TelemetryReading infinite = {23.5f, INFINITY, 1013.2f};
    expect(
        TelemetryService::formatPayload(
            "invalid",
            infinite,
            payload,
            sizeof(payload)) == TelemetryService::PAYLOAD_FORMAT_ERROR,
        "infinite telemetry value is rejected");
    expect(payload[0] == '\0', "infinite payload buffer remains empty");
}

void testInvalidOutput() {
    TelemetryReading reading = {23.5f, 45.0f, 1013.2f};
    char shortPayload[24];
    memset(shortPayload, 'x', sizeof(shortPayload));

    expect(
        TelemetryService::formatPayload(
            "az3166-test",
            reading,
            shortPayload,
            sizeof(shortPayload)) == TelemetryService::PAYLOAD_FORMAT_ERROR,
        "undersized payload buffer is rejected");
    expect(shortPayload[0] == '\0', "rejected payload buffer is cleared");
    expect(
        TelemetryService::formatPayload(
            NULL,
            reading,
            shortPayload,
            sizeof(shortPayload)) == TelemetryService::PAYLOAD_FORMAT_ERROR,
        "null device ID is rejected");
    expect(
        TelemetryService::formatPayload(
            "az3166-test",
            reading,
            NULL,
            AppConfig::TELEMETRY_PAYLOAD_SIZE) ==
            TelemetryService::PAYLOAD_FORMAT_ERROR,
        "null payload buffer is rejected");
    expect(
        TelemetryService::formatPayload(
            "az3166-test",
            reading,
            shortPayload,
            0) == TelemetryService::PAYLOAD_FORMAT_ERROR,
        "zero-sized payload buffer is rejected");
}

void testUnconfiguredIdentity() {
    TelemetryService telemetryService;
    char payload[AppConfig::TELEMETRY_PAYLOAD_SIZE];
    expect(
        telemetryService.buildPayload(payload, sizeof(payload)) ==
            TelemetryService::PAYLOAD_FORMAT_ERROR,
        "payload building requires a configured device ID");
}

void testOnboardSensors() {
    TelemetryService telemetryService;
    telemetryService.begin("sensor-test");
    delay(100);

    TelemetryReading reading;
    bool readSucceeded = telemetryService.read(reading);
    expect(readSucceeded, "onboard telemetry sensors can be read");
    if (readSucceeded) {
        expect(
            isfinite(reading.temperature) &&
                isfinite(reading.humidity) &&
                isfinite(reading.pressure),
            "onboard telemetry readings are finite");
    }

    char payload[AppConfig::TELEMETRY_PAYLOAD_SIZE];
    int length = telemetryService.buildPayload(
        payload,
        sizeof(payload));
    expect(length > 0, "onboard sensors produce a telemetry payload");
    if (length > 0) {
        expect(length == static_cast<int>(strlen(payload)),
               "sensor payload reports its JSON length");
        expect(
            strncmp(payload, "{\"deviceId\":\"sensor-test\",", 26) == 0,
            "sensor payload contains the requested device ID");
    }
}

void setup() {
    Serial.begin(115200);
    while (!Serial);
    delay(3000);

    Serial.println("TEST_SUITE: TelemetryServiceTests");
    testDtostrfRegression();
    testKnownPayload();
    testSignedAndBoundaryValues();
    testRounding();
    testNonFiniteValues();
    testInvalidOutput();
    testUnconfiguredIdentity();
    testOnboardSensors();
}

void loop() {
    Serial.print("TEST_RESULT: ");
    Serial.println(failureCount == 0 ? "PASS" : "FAIL");
    delay(1000);
}