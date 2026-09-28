#ifndef CLOUD_UPLOAD_CONTROLLER_H
#define CLOUD_UPLOAD_CONTROLLER_H

#include <stdint.h>

#include "TelemetryUploadResult.h"
#include "UploadScheduler.h"

class TelemetryUploader;
class NetworkMaintenanceCoordinator;

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

    CloudUploadController(
        UploadScheduler &scheduler,
        TelemetryUploader &uploader,
        NetworkMaintenanceCoordinator &network,
        CloudUploadClock clock = 0);

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
    NetworkMaintenanceCoordinator *network_;
};

#endif