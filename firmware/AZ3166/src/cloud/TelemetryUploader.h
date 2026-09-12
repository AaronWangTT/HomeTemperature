#ifndef TELEMETRY_UPLOADER_H
#define TELEMETRY_UPLOADER_H

#include <stddef.h>

#include "TelemetryUploadResult.h"

class CloudTelemetry;
class TelemetryService;

typedef int (*TelemetryPayloadBuildFunction)(
    char *payload,
    size_t payloadSize);

class TelemetryUploader {
public:
    TelemetryUploader(
        CloudTelemetry &cloudTelemetry,
        TelemetryService &telemetryService);

    TelemetryUploader(
        CloudTelemetry &cloudTelemetry,
        TelemetryPayloadBuildFunction payloadBuildFunction);

    bool isConfigured() const;
    TelemetryUploadResult upload();

private:
    int buildPayload(char *payload, size_t payloadSize);

    CloudTelemetry &cloudTelemetry_;
    TelemetryService *telemetryService_;
    TelemetryPayloadBuildFunction payloadBuildFunction_;
};

#endif