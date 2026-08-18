#ifndef UPLOAD_SCHEDULER_H
#define UPLOAD_SCHEDULER_H

#include <stdint.h>

enum UploadScheduleResult {
    UPLOAD_SCHEDULE_SUCCEEDED,
    UPLOAD_SCHEDULE_RETRYABLE_FAILURE,
    UPLOAD_SCHEDULE_NON_RETRYABLE_FAILURE
};

class UploadScheduler {
public:
    UploadScheduler(
        uint32_t uploadIntervalMs,
        uint32_t retryIntervalMs);

    void requestManualUpload();
    bool togglePaused();
    bool isPaused() const;
    bool isUploadDue(uint32_t now) const;
    void recordResult(
        uint32_t completedAt,
        UploadScheduleResult result);

private:
    uint32_t uploadIntervalMs_;
    uint32_t retryIntervalMs_;
    uint32_t lastAttempt_;
    uint32_t currentDelay_;
    bool paused_;
    bool scheduledUploadsSuppressed_;
    bool manualUploadRequested_;
    bool manualUploadImmediate_;
};

#endif