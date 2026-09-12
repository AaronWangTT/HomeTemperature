#include "UploadScheduler.h"

UploadScheduler::UploadScheduler(
    uint32_t uploadIntervalMs,
    uint32_t retryIntervalMs)
    : uploadIntervalMs_(uploadIntervalMs),
      retryIntervalMs_(retryIntervalMs),
      lastAttempt_(0),
      currentDelay_(0),
      paused_(false),
            scheduledUploadsSuppressed_(false),
      manualUploadRequested_(false),
      manualUploadImmediate_(false) {
}

void UploadScheduler::requestManualUpload() {
    manualUploadRequested_ = true;
    manualUploadImmediate_ = true;
}

bool UploadScheduler::togglePaused() {
    paused_ = !paused_;
    if (!paused_) {
        currentDelay_ = 0;
    }
    return paused_;
}

bool UploadScheduler::isPaused() const {
    return paused_;
}

bool UploadScheduler::isUploadDue(uint32_t now) const {
    bool delayElapsed = now - lastAttempt_ >= currentDelay_;
    bool manualUploadDue =
        manualUploadRequested_ &&
        (manualUploadImmediate_ || delayElapsed);
    bool scheduledUploadDue =
        !paused_ &&
        !scheduledUploadsSuppressed_ &&
        delayElapsed;
    return manualUploadDue || scheduledUploadDue;
}

void UploadScheduler::recordResult(
    uint32_t completedAt,
    UploadScheduleResult result) {
    lastAttempt_ = completedAt;
    manualUploadImmediate_ = false;

    if (result == UPLOAD_SCHEDULE_RETRYABLE_FAILURE) {
        currentDelay_ = retryIntervalMs_;
        return;
    }

    currentDelay_ = uploadIntervalMs_;
    manualUploadRequested_ = false;
    scheduledUploadsSuppressed_ =
        result == UPLOAD_SCHEDULE_NON_RETRYABLE_FAILURE;
}