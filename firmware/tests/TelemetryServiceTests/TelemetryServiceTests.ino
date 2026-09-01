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

void expectInvalidSensorReading(
    const TelemetryReading &reading,
    const char *name) {
    char payload[AppConfig::TELEMETRY_PAYLOAD_SIZE];
    memset(payload, 'x', sizeof(payload));
    expect(
        TelemetryService::formatPayload(
            "invalid",
            reading,
            payload,
            sizeof(payload)) == TelemetryService::PAYLOAD_SENSOR_ERROR,
        name);
    expect(payload[0] == '\0',
           "invalid sensor reading leaves the payload empty");
}

void testSensorReadingValidation() {
    char payload[AppConfig::TELEMETRY_PAYLOAD_SIZE];
    TelemetryReading minimum = {-50.0f, 0.0f, 300.0f};
    expect(
        TelemetryService::formatPayload(
            "minimum",
            minimum,
            payload,
            sizeof(payload)) > 0,
        "server minimum telemetry values are accepted");

    TelemetryReading maximum = {100.0f, 100.0f, 1200.0f};
    expect(
        TelemetryService::formatPayload(
            "maximum",
            maximum,
            payload,
            sizeof(payload)) > 0,
        "server maximum telemetry values are accepted");

    TelemetryReading notANumber = {NAN, 45.0f, 1013.2f};
    expectInvalidSensorReading(
        notANumber,
        "NaN telemetry value is a transient sensor failure");

    TelemetryReading infinite = {23.5f, INFINITY, 1013.2f};
    expectInvalidSensorReading(
        infinite,
        "infinite telemetry value is a transient sensor failure");

    TelemetryReading startupPressure = {31.1f, 70.9f, 253.1f};
    expectInvalidSensorReading(
        startupPressure,
        "observed 253.1 hPa startup outlier is rejected locally");

    TelemetryReading lowTemperature = {-50.1f, 45.0f, 1013.2f};
    expectInvalidSensorReading(
        lowTemperature,
        "temperature below the server range is rejected locally");
    TelemetryReading highTemperature = {100.1f, 45.0f, 1013.2f};
    expectInvalidSensorReading(
        highTemperature,
        "temperature above the server range is rejected locally");
    TelemetryReading lowHumidity = {23.5f, -0.1f, 1013.2f};
    expectInvalidSensorReading(
        lowHumidity,
        "humidity below the server range is rejected locally");
    TelemetryReading highHumidity = {23.5f, 100.1f, 1013.2f};
    expectInvalidSensorReading(
        highHumidity,
        "humidity above the server range is rejected locally");
    TelemetryReading highPressure = {23.5f, 45.0f, 1200.1f};
    expectInvalidSensorReading(
        highPressure,
        "pressure above the server range is rejected locally");
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
    int length = TelemetryService::PAYLOAD_SENSOR_ERROR;
    for (int attempt = 0;
         attempt < 3 &&
             length == TelemetryService::PAYLOAD_SENSOR_ERROR;
         ++attempt) {
        length = telemetryService.buildPayload(
            payload,
            sizeof(payload));
        if (length == TelemetryService::PAYLOAD_SENSOR_ERROR) {
            delay(100);
        }
    }
    expect(length > 0,
           "onboard sensors recover to a valid telemetry payload");
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
    testSensorReadingValidation();
    testInvalidOutput();
    testUnconfiguredIdentity();
    testOnboardSensors();
}

void loop() {
    Serial.print("TEST_RESULT: ");
    Serial.println(failureCount == 0 ? "PASS" : "FAIL");
    delay(1000);
}