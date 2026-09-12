#include <Arduino.h>

#include "src/config/AppConfig.h"
#include "src/cloud/CloudTelemetry.h"
#include "src/telemetry/TelemetryService.h"
#include "src/cloud/TelemetryUploader.h"

const char VALID_API_KEY[] =
    "0123456789ABCDEF0123456789ABCDEF";
const char PLACEHOLDER_API_KEY[] =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
const char EXPECTED_PAYLOAD[] =
    "{\"deviceId\":\"test-device\",\"temperature\":23.5}";

int failureCount = 0;
int builderCallCount = 0;
size_t capturedPayloadSize = 0;

class FakeCloudTelemetryOperations : public CloudTelemetryOperations {
public:
    int sendCallCount = 0;
    size_t capturedRequestLength = 0;
    char capturedRequestPayload[AppConfig::TELEMETRY_PAYLOAD_SIZE] = {};
    TelemetryUploadResult fakeSendResult = {TELEMETRY_UPLOAD_SUCCESS, 201};

    TelemetryUploadResult send(
        const CloudTelemetryRequest &request,
        CloudTelemetryResponseHandler) override {
        ++sendCallCount;
        capturedRequestLength = request.payloadLength;

        size_t copyLength = request.payloadLength;
        if (copyLength >= sizeof(capturedRequestPayload)) {
            copyLength = sizeof(capturedRequestPayload) - 1;
        }
        memcpy(capturedRequestPayload, request.payload, copyLength);
        capturedRequestPayload[copyLength] = '\0';
        return fakeSendResult;
    }
};

void expect(bool condition, const char *name) {
    Serial.print(condition ? "PASS: " : "FAIL: ");
    Serial.println(name);
    if (!condition) {
        ++failureCount;
    }
}

int buildSuccessfulPayload(
    char *payload,
    size_t payloadSize) {
    ++builderCallCount;
    capturedPayloadSize = payloadSize;

    size_t payloadLength = strlen(EXPECTED_PAYLOAD);
    if (payloadLength + 1 > payloadSize) {
        return TelemetryService::PAYLOAD_FORMAT_ERROR;
    }

    memcpy(payload, EXPECTED_PAYLOAD, payloadLength + 1);
    return static_cast<int>(payloadLength);
}

int buildSensorError(
    char *,
    size_t) {
    ++builderCallCount;
    return TelemetryService::PAYLOAD_SENSOR_ERROR;
}

int buildFormatError(
    char *,
    size_t) {
    ++builderCallCount;
    return TelemetryService::PAYLOAD_FORMAT_ERROR;
}

int buildEmptyPayload(
    char *,
    size_t) {
    ++builderCallCount;
    return 0;
}

int buildOversizedPayload(
    char *,
    size_t payloadSize) {
    ++builderCallCount;
    return static_cast<int>(payloadSize);
}

CloudTelemetry createCloudTelemetry(CloudTelemetryOperations &operations) {
    return CloudTelemetry(
        "https://example.test/api/telemetry",
        "test-certificate",
        VALID_API_KEY,
        PLACEHOLDER_API_KEY,
        operations);
}

void resetFakes() {
    builderCallCount = 0;
    capturedPayloadSize = 0;
}

void testSuccessfulUpload() {
    resetFakes();
    FakeCloudTelemetryOperations operations;
    CloudTelemetry cloudTelemetry = createCloudTelemetry(operations);
    TelemetryUploader uploader(
        cloudTelemetry,
        buildSuccessfulPayload);

    TelemetryUploadResult result = uploader.upload();
    expect(result.status == TELEMETRY_UPLOAD_SUCCESS,
           "valid payload is uploaded successfully");
    expect(builderCallCount == 1,
           "upload builds one telemetry payload");
    expect(capturedPayloadSize == AppConfig::TELEMETRY_PAYLOAD_SIZE,
           "upload provides the configured payload capacity");
    expect(operations.sendCallCount == 1,
           "valid payload performs one cloud request");
    expect(operations.capturedRequestLength == strlen(EXPECTED_PAYLOAD),
           "upload preserves the generated payload length");
    expect(strcmp(operations.capturedRequestPayload, EXPECTED_PAYLOAD) == 0,
           "upload preserves the generated payload bytes");
}

void testPayloadFailures() {
    resetFakes();
    FakeCloudTelemetryOperations operations;
    CloudTelemetry cloudTelemetry = createCloudTelemetry(operations);

    TelemetryUploader sensorFailure(
        cloudTelemetry,
        buildSensorError);
    expect(sensorFailure.upload().status ==
               TELEMETRY_UPLOAD_SENSOR_UNAVAILABLE,
           "sensor error fails before cloud transport");
    expect(operations.sendCallCount == 0,
           "sensor error performs no cloud request");

    TelemetryUploader formatFailure(
        cloudTelemetry,
        buildFormatError);
    expect(formatFailure.upload().status ==
               TELEMETRY_UPLOAD_PAYLOAD_INVALID,
           "format error fails before cloud transport");
    expect(operations.sendCallCount == 0,
           "format error performs no cloud request");

    TelemetryUploader emptyPayload(
        cloudTelemetry,
        buildEmptyPayload);
    expect(emptyPayload.upload().status ==
               TELEMETRY_UPLOAD_PAYLOAD_INVALID,
           "zero-length payload fails before cloud transport");
    expect(operations.sendCallCount == 0,
           "zero-length payload performs no cloud request");

    TelemetryUploader oversizedPayload(
        cloudTelemetry,
        buildOversizedPayload);
    expect(oversizedPayload.upload().status ==
               TELEMETRY_UPLOAD_PAYLOAD_INVALID,
           "oversized payload fails before cloud transport");
    expect(operations.sendCallCount == 0,
           "oversized payload performs no cloud request");
}

void testMissingBuilder() {
    resetFakes();
    FakeCloudTelemetryOperations operations;
    CloudTelemetry cloudTelemetry = createCloudTelemetry(operations);
    TelemetryUploader uploader(cloudTelemetry, NULL);

    expect(uploader.upload().status == TELEMETRY_UPLOAD_DISABLED,
           "missing payload builder fails safely");
    expect(operations.sendCallCount == 0,
           "missing builder performs no cloud request");
}

void testCloudFailure() {
    resetFakes();
    FakeCloudTelemetryOperations operations;
    operations.fakeSendResult.status = TELEMETRY_UPLOAD_NETWORK_ERROR;
    operations.fakeSendResult.detailCode = -3001;
    CloudTelemetry cloudTelemetry = createCloudTelemetry(operations);
    TelemetryUploader uploader(
        cloudTelemetry,
        buildSuccessfulPayload);

    TelemetryUploadResult result = uploader.upload();
    expect(result.status == TELEMETRY_UPLOAD_NETWORK_ERROR,
           "cloud transport failure is returned to the scheduler");
    expect(result.detailCode == -3001,
           "cloud transport detail code is preserved");
    expect(builderCallCount == 1,
           "cloud failure occurs after payload construction");
    expect(operations.sendCallCount == 1,
           "cloud failure follows one request attempt");
}

void setup() {
    Serial.begin(115200);
    while (!Serial);
    delay(3000);

    Serial.println("TEST_SUITE: TelemetryUploaderTests");
    testSuccessfulUpload();
    testPayloadFailures();
    testMissingBuilder();
    testCloudFailure();
}

void loop() {
    Serial.print("TEST_RESULT: ");
    Serial.println(failureCount == 0 ? "PASS" : "FAIL");
    delay(1000);
}