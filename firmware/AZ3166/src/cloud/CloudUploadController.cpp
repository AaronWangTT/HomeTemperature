#include "CloudUploadController.h"

#include <Arduino.h>

#include "TelemetryUploader.h"

namespace {

uint32_t readMillis() {
    return millis();
}

}  // namespace

CloudUploadController::CloudUploadController(
    UploadScheduler &scheduler,
    TelemetryUploader &uploader)
    : CloudUploadController(scheduler, uploader, readMillis) {
}

CloudUploadController::CloudUploadController(
    UploadScheduler &scheduler,
    TelemetryUploader &uploader,
    CloudUploadClock clock)
    : scheduler_(scheduler),
      uploader_(uploader),
      clock_(clock) {
}

bool CloudUploadController::requestManualUpload() {
    if (!uploader_.isConfigured()) {
        Serial.println("Manual cloud upload is not configured");
        return false;
    }

    scheduler_.requestManualUpload();
    Serial.println("Manual cloud upload queued");
    return true;
}

bool CloudUploadController::togglePaused() {
    bool paused = scheduler_.togglePaused();
    if (paused) {
        Serial.println("Scheduled cloud uploads paused");
    } else {
        Serial.println("Scheduled cloud uploads resumed");
    }
    return paused;
}

bool CloudUploadController::isPaused() const {
    return scheduler_.isPaused();
}

void CloudUploadController::update(bool prerequisitesReady) {
    if (!prerequisitesReady ||
        !uploader_.isConfigured() ||
        clock_ == NULL) {
        return;
    }

    uint32_t now = clock_();
    if (!scheduler_.isUploadDue(now)) {
        return;
    }

    TelemetryUploadResult result = uploader_.upload();
    scheduler_.recordResult(
        clock_(),
        scheduleResultFor(result.status));
}

UploadScheduleResult CloudUploadController::scheduleResultFor(
    TelemetryUploadStatus status) {
    switch (status) {
        case TELEMETRY_UPLOAD_SUCCESS:
            return UPLOAD_SCHEDULE_SUCCEEDED;

        case TELEMETRY_UPLOAD_SENSOR_UNAVAILABLE:
        case TELEMETRY_UPLOAD_NETWORK_ERROR:
        case TELEMETRY_UPLOAD_HTTP_RETRYABLE:
            return UPLOAD_SCHEDULE_RETRYABLE_FAILURE;

        case TELEMETRY_UPLOAD_PAYLOAD_INVALID:
        case TELEMETRY_UPLOAD_HTTP_REJECTED:
        case TELEMETRY_UPLOAD_DISABLED:
        default:
            return UPLOAD_SCHEDULE_NON_RETRYABLE_FAILURE;
    }
}