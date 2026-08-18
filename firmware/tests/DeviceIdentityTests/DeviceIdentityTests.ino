#include <Arduino.h>

#include "AppConfig.h"
#include "DeviceIdentity.h"

int failureCount = 0;

void expect(bool condition, const char *name) {
    Serial.print(condition ? "PASS: " : "FAIL: ");
    Serial.println(name);
    if (!condition) {
        ++failureCount;
    }
}

void testKnownDeviceId() {
    char output[DeviceIdentity::DEVICE_ID_SIZE];
    bool formatted = DeviceIdentity::format(
        output,
        sizeof(output),
        0x00112233UL,
        0x44556677UL,
        0x8899AABBUL);

    expect(formatted, "known UID fits the identity buffer");
    expect(
        strcmp(output, "az3166-00112233445566778899AABB") == 0,
        "synthetic UID preserves the deployed device ID format");
}

void testUidPadding() {
    char output[DeviceIdentity::DEVICE_ID_SIZE];
    bool formatted = DeviceIdentity::format(
        output,
        sizeof(output),
        0,
        1,
        0xFFFFFFFFUL);

    expect(formatted, "UID boundary values fit the identity buffer");
    expect(
        strcmp(output, "az3166-0000000000000001FFFFFFFF") == 0,
        "UID words use eight uppercase hexadecimal digits");
}

void testInvalidBuffers() {
    char shortOutput[DeviceIdentity::DEVICE_ID_SIZE - 1];
    memset(shortOutput, 'x', sizeof(shortOutput));

    expect(
        !DeviceIdentity::format(shortOutput, sizeof(shortOutput), 1, 2, 3),
        "undersized output buffer is rejected");
    expect(shortOutput[0] == '\0', "rejected output is cleared");
    expect(
        !DeviceIdentity::format(NULL, DeviceIdentity::DEVICE_ID_SIZE, 1, 2, 3),
        "null output buffer is rejected");
    expect(
        !DeviceIdentity::format(shortOutput, 0, 1, 2, 3),
        "zero-sized output buffer is rejected");
}

void testUninitializedDeviceId() {
    DeviceIdentity identity;

    expect(
        !identity.isInitialized(),
        "new identity reports that it is not initialized");
    expect(
        strcmp(
            identity.get(),
            "az3166-FFFFFFFFFFFFFFFFFFFFFFFF") == 0,
        "uninitialized identity returns the fallback device ID");
    expect(
        strlen(identity.get()) == DeviceIdentity::DEVICE_ID_SIZE - 1,
        "fallback device ID fills the expected buffer");
}

void testHardwareDeviceId() {
    DeviceIdentity identity;

    expect(
        identity.begin(),
        "hardware UID initializes the stored device ID");
    expect(
        identity.isInitialized(),
        "initialized identity reports its state");

    const char *deviceId = identity.get();
    expect(deviceId != NULL, "stored device ID is available");
    expect(
        strlen(deviceId) == DeviceIdentity::DEVICE_ID_SIZE - 1,
        "stored device ID fills the expected buffer");
    expect(
        strncmp(deviceId, "az3166-", 7) == 0,
        "stored device ID uses the AZ3166 prefix");
}

void testApplicationConfig() {
    expect(
        AppConfig::CLOUD_UPLOAD_INTERVAL_MS == 5UL * 60UL * 1000UL,
        "scheduled cloud upload interval is five minutes");
    expect(
        AppConfig::CLOUD_RETRY_INTERVAL_MS == 15UL * 1000UL,
        "failed cloud upload retry interval is fifteen seconds");
    expect(
        AppConfig::CLOUD_UPLOAD_INTERVAL_MS >
            AppConfig::CLOUD_RETRY_INTERVAL_MS,
        "retry interval is shorter than the normal upload interval");
    expect(
        AppConfig::TELEMETRY_PAYLOAD_SIZE > DeviceIdentity::DEVICE_ID_SIZE,
        "telemetry payload buffer can contain the device ID");
    expect(
        strncmp(AppConfig::CLOUD_TELEMETRY_URL, "https://", 8) == 0,
        "cloud telemetry endpoint requires HTTPS");
}

void setup() {
    Serial.begin(115200);
    while (!Serial);
    delay(3000);

    Serial.println("TEST_SUITE: DeviceIdentityTests");
    testKnownDeviceId();
    testUidPadding();
    testInvalidBuffers();
    testUninitializedDeviceId();
    testHardwareDeviceId();
    testApplicationConfig();
}

void loop() {
    Serial.print("TEST_RESULT: ");
    Serial.println(failureCount == 0 ? "PASS" : "FAIL");
    delay(1000);
}