#ifndef CLOUD_UPLOAD_CONTROLLER_H
#define CLOUD_UPLOAD_CONTROLLER_H

#include <stdint.h>

#include "TelemetryUploadResult.h"
#include "UploadScheduler.h"

class TelemetryUploader;

typedef uint32_t (*CloudUploadClock)();

class CloudUploadController {
public:
    CloudUploadController(
        UploadScheduler &scheduler,
        TelemetryUploader &uploader);

    CloudUploadController(
        UploadScheduler &scheduler,
        TelemetryUploader &uploader,
        CloudUploadClock clock);

    bool requestManualUpload();
    bool togglePaused();
    bool isPaused() const;
    void update(bool prerequisitesReady);

    static UploadScheduleResult scheduleResultFor(
        TelemetryUploadStatus status);

private:
    UploadScheduler &scheduler_;
    TelemetryUploader &uploader_;
    CloudUploadClock clock_;
};

#endif