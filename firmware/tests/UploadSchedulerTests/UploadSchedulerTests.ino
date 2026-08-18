#include <Arduino.h>

#include "AppConfig.h"
#include "UploadScheduler.h"

int failureCount = 0;

void expect(bool condition, const char *name) {
    Serial.print(condition ? "PASS: " : "FAIL: ");
    Serial.println(name);
    if (!condition) {
        ++failureCount;
    }
}

UploadScheduler createScheduler() {
    return UploadScheduler(
        AppConfig::CLOUD_UPLOAD_INTERVAL_MS,
        AppConfig::CLOUD_RETRY_INTERVAL_MS);
}

void testStartupAndSuccessInterval() {
    UploadScheduler scheduler = createScheduler();

    expect(scheduler.isUploadDue(0), "startup upload is immediately due");
       scheduler.recordResult(1000, UPLOAD_SCHEDULE_SUCCEEDED);
    expect(!scheduler.isUploadDue(1000),
           "successful upload starts the normal interval");
    expect(!scheduler.isUploadDue(300999),
           "successful upload is not due before five minutes");
    expect(scheduler.isUploadDue(301000),
           "successful upload is due at five minutes");
}

void testFailureRetryInterval() {
    UploadScheduler scheduler = createScheduler();

       scheduler.recordResult(
              1000,
              UPLOAD_SCHEDULE_RETRYABLE_FAILURE);
    expect(!scheduler.isUploadDue(15999),
           "failed upload is not retried before fifteen seconds");
    expect(scheduler.isUploadDue(16000),
           "failed upload is retried at fifteen seconds");
       scheduler.recordResult(16000, UPLOAD_SCHEDULE_SUCCEEDED);
    expect(!scheduler.isUploadDue(16001),
           "successful retry returns to the normal interval");
}

void testPauseAndResume() {
    UploadScheduler scheduler = createScheduler();

    expect(scheduler.togglePaused(), "first toggle pauses uploads");
    expect(scheduler.isPaused(), "scheduler reports the paused state");
    expect(!scheduler.isUploadDue(0),
           "pause suppresses the startup scheduled upload");
    expect(!scheduler.togglePaused(), "second toggle resumes uploads");
    expect(!scheduler.isPaused(), "scheduler reports the resumed state");
    expect(scheduler.isUploadDue(0),
           "resume makes a scheduled upload immediately due");
}

void testManualUploadWhilePaused() {
    UploadScheduler scheduler = createScheduler();
    scheduler.togglePaused();

    scheduler.requestManualUpload();
    expect(scheduler.isUploadDue(100),
           "manual upload remains available while paused");
       scheduler.recordResult(100, UPLOAD_SCHEDULE_SUCCEEDED);
    expect(!scheduler.isUploadDue(300100),
           "successful manual upload does not bypass pause later");
}

void testManualFailureWhilePaused() {
    UploadScheduler scheduler = createScheduler();
    scheduler.togglePaused();
    scheduler.requestManualUpload();
       scheduler.recordResult(
              100,
              UPLOAD_SCHEDULE_RETRYABLE_FAILURE);

    expect(!scheduler.isUploadDue(15099),
           "failed manual upload waits fifteen seconds while paused");
    expect(scheduler.isUploadDue(15100),
           "failed manual upload retries while paused");
       scheduler.recordResult(15100, UPLOAD_SCHEDULE_SUCCEEDED);
    expect(!scheduler.isUploadDue(315100),
           "successful manual retry clears the pending request");
}

void testNewManualRequestForcesImmediateRetry() {
    UploadScheduler scheduler = createScheduler();
    scheduler.togglePaused();
    scheduler.requestManualUpload();
       scheduler.recordResult(
              100,
              UPLOAD_SCHEDULE_RETRYABLE_FAILURE);

    expect(!scheduler.isUploadDue(1000),
           "manual failure initially observes retry delay");
    scheduler.requestManualUpload();
    expect(scheduler.isUploadDue(1000),
           "new manual request bypasses the existing retry delay");
}

void testScheduledFailureCanBePaused() {
    UploadScheduler scheduler = createScheduler();
       scheduler.recordResult(
              0,
              UPLOAD_SCHEDULE_RETRYABLE_FAILURE);
    scheduler.togglePaused();

    expect(!scheduler.isUploadDue(AppConfig::CLOUD_RETRY_INTERVAL_MS),
           "pause suppresses a scheduled failure retry");
    scheduler.togglePaused();
    expect(scheduler.isUploadDue(AppConfig::CLOUD_RETRY_INTERVAL_MS),
           "resume makes the scheduled retry immediately due");
}

void testManualRequestDuringNormalInterval() {
    UploadScheduler scheduler = createScheduler();
       scheduler.recordResult(0, UPLOAD_SCHEDULE_SUCCEEDED);

    expect(!scheduler.isUploadDue(1000),
           "normal interval is active after success");
    scheduler.requestManualUpload();
    expect(scheduler.isUploadDue(1000),
           "manual request bypasses the normal interval");
       scheduler.recordResult(1000, UPLOAD_SCHEDULE_SUCCEEDED);
    expect(!scheduler.isUploadDue(300999),
           "manual success restarts the normal interval");
    expect(scheduler.isUploadDue(301000),
           "normal interval is measured from manual success");
}

void testMillisWraparound() {
    UploadScheduler scheduler = createScheduler();
    uint32_t attemptTime = 0xFFFFFF00UL;
       scheduler.recordResult(
              attemptTime,
              UPLOAD_SCHEDULE_SUCCEEDED);

    uint32_t beforeDue =
        attemptTime + AppConfig::CLOUD_UPLOAD_INTERVAL_MS - 1;
    uint32_t dueTime =
        attemptTime + AppConfig::CLOUD_UPLOAD_INTERVAL_MS;
    expect(!scheduler.isUploadDue(beforeDue),
           "wrapped normal interval is not due one millisecond early");
    expect(scheduler.isUploadDue(dueTime),
           "wrapped normal interval is due on time");
}

void testNonRetryableFailureSuppressesScheduledUploads() {
       UploadScheduler scheduler = createScheduler();

       scheduler.recordResult(
              1000,
              UPLOAD_SCHEDULE_NON_RETRYABLE_FAILURE);
       expect(
              !scheduler.isUploadDue(
                     1000 + AppConfig::CLOUD_RETRY_INTERVAL_MS),
              "non-retryable failure skips the short retry interval");
       expect(
              !scheduler.isUploadDue(
                     1000 + AppConfig::CLOUD_UPLOAD_INTERVAL_MS),
              "non-retryable failure suppresses scheduled uploads");

       scheduler.requestManualUpload();
       expect(
              scheduler.isUploadDue(2000),
              "manual request bypasses non-retryable suppression");
       scheduler.recordResult(
              2000,
              UPLOAD_SCHEDULE_NON_RETRYABLE_FAILURE);
       expect(
              !scheduler.isUploadDue(
                     2000 + AppConfig::CLOUD_UPLOAD_INTERVAL_MS),
              "non-retryable manual failure clears the request");

       scheduler.requestManualUpload();
       scheduler.recordResult(3000, UPLOAD_SCHEDULE_SUCCEEDED);
       expect(
              scheduler.isUploadDue(
                     3000 + AppConfig::CLOUD_UPLOAD_INTERVAL_MS),
              "successful manual upload restores scheduled uploads");
}

void setup() {
    Serial.begin(115200);
    while (!Serial);
    delay(3000);

    Serial.println("TEST_SUITE: UploadSchedulerTests");
    expect(
        AppConfig::CLOUD_UPLOAD_INTERVAL_MS == 300000UL,
        "normal upload interval is five minutes");
    expect(
        AppConfig::CLOUD_RETRY_INTERVAL_MS == 15000UL,
        "failed upload retry interval is fifteen seconds");
    testStartupAndSuccessInterval();
    testFailureRetryInterval();
    testPauseAndResume();
    testManualUploadWhilePaused();
    testManualFailureWhilePaused();
    testNewManualRequestForcesImmediateRetry();
    testScheduledFailureCanBePaused();
    testManualRequestDuringNormalInterval();
    testMillisWraparound();
       testNonRetryableFailureSuppressesScheduledUploads();
}

void loop() {
    Serial.print("TEST_RESULT: ");
    Serial.println(failureCount == 0 ? "PASS" : "FAIL");
    delay(1000);
}