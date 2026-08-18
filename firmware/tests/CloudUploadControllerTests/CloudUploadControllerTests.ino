#include <Arduino.h>

#include "AppConfig.h"
#include "CloudTelemetry.h"
#include "CloudUploadController.h"
#include "TelemetryService.h"
#include "TelemetryUploader.h"
#include "UploadScheduler.h"

const char VALID_API_KEY[] =
    "0123456789ABCDEF0123456789ABCDEF";
const char PLACEHOLDER_API_KEY[] =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

int failureCount;
int builderCallCount;
int sendCallCount;
uint32_t fakeNow;
uint32_t fakeCompletionTime;
TelemetryUploadResult fakeUploadResult;

void expect(bool condition, const char *name) {
    Serial.print(condition ? "PASS: " : "FAIL: ");
    Serial.println(name);
    if (!condition) {
        ++failureCount;
    }
}

uint32_t readFakeClock() {
    return fakeNow;
}

int buildFakePayload(char *payload, size_t payloadSize) {
    ++builderCallCount;
    if (payload == NULL || payloadSize < 3) {
        return TelemetryService::PAYLOAD_FORMAT_ERROR;
    }

    payload[0] = '{';
    payload[1] = '}';
    payload[2] = '\0';
    return 2;
}

TelemetryUploadResult sendFakeRequest(
    const CloudTelemetryRequest &,
    CloudTelemetryResponseHandler) {
    ++sendCallCount;
    fakeNow = fakeCompletionTime;
    return fakeUploadResult;
}

CloudTelemetry createCloudTelemetry(const char *apiKey) {
    CloudTelemetryOperations operations = {
        sendFakeRequest
    };
    return CloudTelemetry(
        "https://example.test/api/telemetry",
        "test-certificate",
        apiKey,
        PLACEHOLDER_API_KEY,
        operations);
}

UploadScheduler createScheduler() {
    return UploadScheduler(
        AppConfig::CLOUD_UPLOAD_INTERVAL_MS,
        AppConfig::CLOUD_RETRY_INTERVAL_MS);
}

void resetFakes() {
    builderCallCount = 0;
    sendCallCount = 0;
    fakeNow = 0;
    fakeCompletionTime = 0;
    fakeUploadResult.status = TELEMETRY_UPLOAD_SUCCESS;
    fakeUploadResult.detailCode = 201;
}

void testPrerequisitesGateStartupUpload() {
    resetFakes();
    UploadScheduler scheduler = createScheduler();
    CloudTelemetry cloudTelemetry =
        createCloudTelemetry(VALID_API_KEY);
    TelemetryUploader uploader(
        cloudTelemetry,
        buildFakePayload);
    CloudUploadController controller(
        scheduler,
        uploader,
        readFakeClock);

    fakeNow = 1000;
    fakeCompletionTime = 2000;
    controller.update(false);
    expect(builderCallCount == 0,
           "offline state does not build a payload");
    expect(sendCallCount == 0,
           "offline state does not call cloud transport");

    controller.update(true);
    expect(builderCallCount == 1,
           "ready state builds the startup payload");
    expect(sendCallCount == 1,
           "ready state performs the startup upload");
}

void testManualUploadWaitsForPrerequisites() {
    resetFakes();
    UploadScheduler scheduler = createScheduler();
    CloudTelemetry cloudTelemetry =
        createCloudTelemetry(VALID_API_KEY);
    TelemetryUploader uploader(
        cloudTelemetry,
        buildFakePayload);
    CloudUploadController controller(
        scheduler,
        uploader,
        readFakeClock);

    expect(controller.togglePaused(),
           "controller pauses scheduled uploads");
    expect(controller.requestManualUpload(),
           "configured controller queues a manual upload");

    fakeNow = 5000;
    fakeCompletionTime = 6000;
    controller.update(false);
    expect(sendCallCount == 0,
           "offline manual request remains pending");

    controller.update(true);
    expect(sendCallCount == 1,
           "pending manual request runs when ready");

    fakeNow =
        fakeCompletionTime + AppConfig::CLOUD_UPLOAD_INTERVAL_MS;
    controller.update(true);
    expect(sendCallCount == 1,
           "pause still suppresses scheduled uploads after manual success");
}

void testUnconfiguredUploadIsRejected() {
    resetFakes();
    UploadScheduler scheduler = createScheduler();
    CloudTelemetry cloudTelemetry =
        createCloudTelemetry("short-key");
    TelemetryUploader uploader(
        cloudTelemetry,
        buildFakePayload);
    CloudUploadController controller(
        scheduler,
        uploader,
        readFakeClock);

    expect(!controller.requestManualUpload(),
           "unconfigured controller rejects a manual request");
    controller.update(true);
    expect(builderCallCount == 0,
           "unconfigured controller does not build a payload");
    expect(sendCallCount == 0,
           "unconfigured controller does not call transport");
}

void testRetryDelayStartsAtCompletion() {
    resetFakes();
    UploadScheduler scheduler = createScheduler();
    CloudTelemetry cloudTelemetry =
        createCloudTelemetry(VALID_API_KEY);
    TelemetryUploader uploader(
        cloudTelemetry,
        buildFakePayload);
    CloudUploadController controller(
        scheduler,
        uploader,
        readFakeClock);

    fakeNow = 1000;
    fakeCompletionTime = 21000;
    fakeUploadResult.status = TELEMETRY_UPLOAD_NETWORK_ERROR;
    fakeUploadResult.detailCode = -3001;
    controller.update(true);
    expect(sendCallCount == 1,
           "retryable failure performs one upload attempt");
    expect(fakeNow == 21000,
           "fake upload advances time to completion");

    fakeUploadResult.status = TELEMETRY_UPLOAD_SUCCESS;
    fakeUploadResult.detailCode = 201;
    fakeNow =
        fakeCompletionTime + AppConfig::CLOUD_RETRY_INTERVAL_MS - 1;
    controller.update(true);
    expect(sendCallCount == 1,
           "retry does not use the upload start time");

    fakeNow =
        fakeCompletionTime + AppConfig::CLOUD_RETRY_INTERVAL_MS;
    fakeCompletionTime = fakeNow + 500;
    controller.update(true);
    expect(sendCallCount == 2,
           "retry becomes due from the upload completion time");
}

void testNonRetryableFailureRequiresManualRecovery() {
    resetFakes();
    UploadScheduler scheduler = createScheduler();
    CloudTelemetry cloudTelemetry =
        createCloudTelemetry(VALID_API_KEY);
    TelemetryUploader uploader(
        cloudTelemetry,
        buildFakePayload);
    CloudUploadController controller(
        scheduler,
        uploader,
        readFakeClock);

    fakeNow = 1000;
    fakeCompletionTime = 2000;
    fakeUploadResult.status = TELEMETRY_UPLOAD_HTTP_REJECTED;
    fakeUploadResult.detailCode = 401;
    controller.update(true);
    expect(sendCallCount == 1,
           "HTTP rejection performs one upload attempt");

    fakeNow =
        fakeCompletionTime +
        (2 * AppConfig::CLOUD_UPLOAD_INTERVAL_MS);
    controller.update(true);
    expect(sendCallCount == 1,
           "HTTP rejection suppresses scheduled retries");

    expect(controller.requestManualUpload(),
           "manual request can probe recovery after rejection");
    fakeUploadResult.status = TELEMETRY_UPLOAD_SUCCESS;
    fakeUploadResult.detailCode = 201;
    fakeCompletionTime = fakeNow + 1000;
    controller.update(true);
    expect(sendCallCount == 2,
           "manual recovery request performs a new upload");

    fakeNow =
        fakeCompletionTime + AppConfig::CLOUD_UPLOAD_INTERVAL_MS;
    fakeCompletionTime = fakeNow + 1000;
    controller.update(true);
    expect(sendCallCount == 3,
           "manual success restores scheduled uploads");
}

void testUploadResultClassification() {
    expect(
        CloudUploadController::scheduleResultFor(
            TELEMETRY_UPLOAD_SUCCESS) ==
            UPLOAD_SCHEDULE_SUCCEEDED,
        "success maps to the normal upload interval");
    expect(
        CloudUploadController::scheduleResultFor(
            TELEMETRY_UPLOAD_SENSOR_UNAVAILABLE) ==
            UPLOAD_SCHEDULE_RETRYABLE_FAILURE,
        "sensor failure maps to a retryable result");
    expect(
        CloudUploadController::scheduleResultFor(
            TELEMETRY_UPLOAD_NETWORK_ERROR) ==
            UPLOAD_SCHEDULE_RETRYABLE_FAILURE,
        "network failure maps to a retryable result");
    expect(
        CloudUploadController::scheduleResultFor(
            TELEMETRY_UPLOAD_HTTP_RETRYABLE) ==
            UPLOAD_SCHEDULE_RETRYABLE_FAILURE,
        "retryable HTTP status maps to a retryable result");
    expect(
        CloudUploadController::scheduleResultFor(
            TELEMETRY_UPLOAD_PAYLOAD_INVALID) ==
            UPLOAD_SCHEDULE_NON_RETRYABLE_FAILURE,
        "payload error maps to a non-retryable result");
    expect(
        CloudUploadController::scheduleResultFor(
            TELEMETRY_UPLOAD_HTTP_REJECTED) ==
            UPLOAD_SCHEDULE_NON_RETRYABLE_FAILURE,
        "HTTP rejection maps to a non-retryable result");
    expect(
        CloudUploadController::scheduleResultFor(
            TELEMETRY_UPLOAD_DISABLED) ==
            UPLOAD_SCHEDULE_NON_RETRYABLE_FAILURE,
        "disabled upload maps to a non-retryable result");
}

void setup() {
    Serial.begin(115200);
    while (!Serial);
    delay(3000);

    Serial.println("TEST_SUITE: CloudUploadControllerTests");
    expect(
        AppConfig::CLOUD_UPLOAD_INTERVAL_MS == 300000UL,
        "normal upload interval is five minutes");
    expect(
        AppConfig::CLOUD_RETRY_INTERVAL_MS == 15000UL,
        "retry interval is fifteen seconds");
    testPrerequisitesGateStartupUpload();
    testManualUploadWaitsForPrerequisites();
    testUnconfiguredUploadIsRejected();
    testRetryDelayStartsAtCompletion();
    testNonRetryableFailureRequiresManualRecovery();
    testUploadResultClassification();
}

void loop() {
    Serial.print("TEST_RESULT: ");
    Serial.println(failureCount == 0 ? "PASS" : "FAIL");
    delay(1000);
}