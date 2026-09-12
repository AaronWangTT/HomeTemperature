#include <Arduino.h>

#include "src/config/AppConfig.h"
#include "src/cloud/CloudTelemetry.h"
#include "src/cloud/CloudUploadController.h"
#include "src/telemetry/TelemetryService.h"
#include "src/cloud/TelemetryUploader.h"
#include "src/cloud/UploadScheduler.h"

const char VALID_API_KEY[] =
    "0123456789ABCDEF0123456789ABCDEF";
const char PLACEHOLDER_API_KEY[] =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

int failureCount;
int builderCallCount;
uint32_t fakeNow;

class FakeCloudTelemetryOperations : public CloudTelemetryOperations {
public:
    explicit FakeCloudTelemetryOperations(uint32_t &clock) : clock_(clock) {}

    int sendCallCount = 0;
    uint32_t fakeCompletionTime = 0;
    TelemetryUploadResult fakeUploadResult = {TELEMETRY_UPLOAD_SUCCESS, 201};

    TelemetryUploadResult send(
        const CloudTelemetryRequest &,
        CloudTelemetryResponseHandler) override {
        ++sendCallCount;
        clock_ = fakeCompletionTime;
        return fakeUploadResult;
    }

private:
    uint32_t &clock_;
};

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

CloudTelemetry createCloudTelemetry(const char *apiKey, CloudTelemetryOperations &operations) {
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
    fakeNow = 0;
}

void testPrerequisitesGateStartupUpload() {
    resetFakes();
    FakeCloudTelemetryOperations operations(fakeNow);
    UploadScheduler scheduler = createScheduler();
    CloudTelemetry cloudTelemetry =
        createCloudTelemetry(VALID_API_KEY, operations);
    TelemetryUploader uploader(
        cloudTelemetry,
        buildFakePayload);
    CloudUploadController controller(
        scheduler,
        uploader,
        readFakeClock);

    fakeNow = 1000;
    operations.fakeCompletionTime = 2000;
    controller.update(false);
    expect(builderCallCount == 0,
           "offline state does not build a payload");
    expect(operations.sendCallCount == 0,
           "offline state does not call cloud transport");

    controller.update(true);
    expect(builderCallCount == 1,
           "ready state builds the startup payload");
    expect(operations.sendCallCount == 1,
           "ready state performs the startup upload");
}

void testManualUploadWaitsForPrerequisites() {
    resetFakes();
    FakeCloudTelemetryOperations operations(fakeNow);
    UploadScheduler scheduler = createScheduler();
    CloudTelemetry cloudTelemetry =
        createCloudTelemetry(VALID_API_KEY, operations);
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
    operations.fakeCompletionTime = 6000;
    controller.update(false);
    expect(operations.sendCallCount == 0,
           "offline manual request remains pending");

    controller.update(true);
    expect(operations.sendCallCount == 1,
           "pending manual request runs when ready");

    fakeNow =
        operations.fakeCompletionTime + AppConfig::CLOUD_UPLOAD_INTERVAL_MS;
    controller.update(true);
    expect(operations.sendCallCount == 1,
           "pause still suppresses scheduled uploads after manual success");
}

void testUnconfiguredUploadIsRejected() {
    resetFakes();
    FakeCloudTelemetryOperations operations(fakeNow);
    UploadScheduler scheduler = createScheduler();
    CloudTelemetry cloudTelemetry =
        createCloudTelemetry("short-key", operations);
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
    expect(operations.sendCallCount == 0,
           "unconfigured controller does not call transport");
}

void testRetryDelayStartsAtCompletion() {
    resetFakes();
    FakeCloudTelemetryOperations operations(fakeNow);
    UploadScheduler scheduler = createScheduler();
    CloudTelemetry cloudTelemetry =
        createCloudTelemetry(VALID_API_KEY, operations);
    TelemetryUploader uploader(
        cloudTelemetry,
        buildFakePayload);
    CloudUploadController controller(
        scheduler,
        uploader,
        readFakeClock);

    fakeNow = 1000;
    operations.fakeCompletionTime = 21000;
    operations.fakeUploadResult.status = TELEMETRY_UPLOAD_NETWORK_ERROR;
    operations.fakeUploadResult.detailCode = -3001;
    controller.update(true);
    expect(operations.sendCallCount == 1,
           "retryable failure performs one upload attempt");
    expect(fakeNow == 21000,
           "fake upload advances time to completion");

    operations.fakeUploadResult.status = TELEMETRY_UPLOAD_SUCCESS;
    operations.fakeUploadResult.detailCode = 201;
    fakeNow =
        operations.fakeCompletionTime + AppConfig::CLOUD_RETRY_INTERVAL_MS - 1;
    controller.update(true);
    expect(operations.sendCallCount == 1,
           "retry does not use the upload start time");

    fakeNow =
        operations.fakeCompletionTime + AppConfig::CLOUD_RETRY_INTERVAL_MS;
    operations.fakeCompletionTime = fakeNow + 500;
    controller.update(true);
    expect(operations.sendCallCount == 2,
           "retry becomes due from the upload completion time");
}

void testHttp422RetriesAutomatically() {
    resetFakes();
    FakeCloudTelemetryOperations operations(fakeNow);
    UploadScheduler scheduler = createScheduler();
    CloudTelemetry cloudTelemetry =
        createCloudTelemetry(VALID_API_KEY, operations);
    TelemetryUploader uploader(
        cloudTelemetry,
        buildFakePayload);
    CloudUploadController controller(
        scheduler,
        uploader,
        readFakeClock);

    fakeNow = 1000;
    operations.fakeCompletionTime = 2000;
    controller.update(true);
    expect(operations.sendCallCount == 1,
           "initial telemetry upload succeeds");

    fakeNow =
        operations.fakeCompletionTime + AppConfig::CLOUD_UPLOAD_INTERVAL_MS;
    operations.fakeCompletionTime = fakeNow + 1000;
    operations.fakeUploadResult.status = TELEMETRY_UPLOAD_HTTP_RETRYABLE;
    operations.fakeUploadResult.detailCode = 422;
    controller.update(true);
    expect(operations.sendCallCount == 2,
           "HTTP 422 records a retryable upload failure");

    fakeNow =
        operations.fakeCompletionTime + AppConfig::CLOUD_RETRY_INTERVAL_MS - 1;
    controller.update(true);
    expect(operations.sendCallCount == 2,
           "HTTP 422 waits for the retry interval");

    fakeNow =
        operations.fakeCompletionTime + AppConfig::CLOUD_RETRY_INTERVAL_MS;
    operations.fakeCompletionTime = fakeNow + 1000;
    operations.fakeUploadResult.status = TELEMETRY_UPLOAD_SUCCESS;
    operations.fakeUploadResult.detailCode = 201;
    controller.update(true);
    expect(operations.sendCallCount == 3,
           "HTTP 422 retries without requiring Button A");
}

void testNonRetryableFailureRequiresManualRecovery() {
    resetFakes();
    FakeCloudTelemetryOperations operations(fakeNow);
    UploadScheduler scheduler = createScheduler();
    CloudTelemetry cloudTelemetry =
        createCloudTelemetry(VALID_API_KEY, operations);
    TelemetryUploader uploader(
        cloudTelemetry,
        buildFakePayload);
    CloudUploadController controller(
        scheduler,
        uploader,
        readFakeClock);

    fakeNow = 1000;
    operations.fakeCompletionTime = 2000;
    operations.fakeUploadResult.status = TELEMETRY_UPLOAD_HTTP_REJECTED;
    operations.fakeUploadResult.detailCode = 401;
    controller.update(true);
    expect(operations.sendCallCount == 1,
           "HTTP rejection performs one upload attempt");

    fakeNow =
        operations.fakeCompletionTime +
        (2 * AppConfig::CLOUD_UPLOAD_INTERVAL_MS);
    controller.update(true);
    expect(operations.sendCallCount == 1,
           "HTTP rejection suppresses scheduled retries");

    expect(controller.requestManualUpload(),
           "manual request can probe recovery after rejection");
    operations.fakeUploadResult.status = TELEMETRY_UPLOAD_SUCCESS;
    operations.fakeUploadResult.detailCode = 201;
    operations.fakeCompletionTime = fakeNow + 1000;
    controller.update(true);
    expect(operations.sendCallCount == 2,
           "manual recovery request performs a new upload");

    fakeNow =
        operations.fakeCompletionTime + AppConfig::CLOUD_UPLOAD_INTERVAL_MS;
    operations.fakeCompletionTime = fakeNow + 1000;
    controller.update(true);
    expect(operations.sendCallCount == 3,
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
    testHttp422RetriesAutomatically();
    testNonRetryableFailureRequiresManualRecovery();
    testUploadResultClassification();
}

void loop() {
    Serial.print("TEST_RESULT: ");
    Serial.println(failureCount == 0 ? "PASS" : "FAIL");
    delay(1000);
}