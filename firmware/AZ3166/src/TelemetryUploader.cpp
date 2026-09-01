#include "TelemetryUploader.h"

#include <Arduino.h>

#include "AppConfig.h"
#include "CloudTelemetry.h"
#include "TelemetryService.h"

TelemetryUploader::TelemetryUploader(
        CloudTelemetry &cloudTelemetry,
        TelemetryService &telemetryService)
        : cloudTelemetry_(cloudTelemetry),
            telemetryService_(&telemetryService),
            payloadBuildFunction_(NULL) {
}

TelemetryUploader::TelemetryUploader(
    CloudTelemetry &cloudTelemetry,
        TelemetryPayloadBuildFunction payloadBuildFunction)
    : cloudTelemetry_(cloudTelemetry),
            telemetryService_(NULL),
            payloadBuildFunction_(payloadBuildFunction) {
}

bool TelemetryUploader::isConfigured() const {
    return cloudTelemetry_.isConfigured();
}

TelemetryUploadResult TelemetryUploader::upload() {
    if (telemetryService_ == NULL &&
        payloadBuildFunction_ == NULL) {
        Serial.println("Cloud upload skipped: payload builder is unavailable");
        return {TELEMETRY_UPLOAD_DISABLED, 0};
    }

    char payload[AppConfig::TELEMETRY_PAYLOAD_SIZE];
    int payloadLength = buildPayload(
        payload,
        sizeof(payload));
    if (payloadLength == TelemetryService::PAYLOAD_SENSOR_ERROR) {
        Serial.println(
            "Cloud upload skipped: sensor reading unavailable or invalid");
        return {
            TELEMETRY_UPLOAD_SENSOR_UNAVAILABLE,
            payloadLength
        };
    }
    if (payloadLength <= 0 ||
        static_cast<size_t>(payloadLength) >= sizeof(payload)) {
        Serial.println("Cloud upload skipped: payload buffer is too small");
        return {
            TELEMETRY_UPLOAD_PAYLOAD_INVALID,
            payloadLength
        };
    }

    return cloudTelemetry_.upload(
        payload,
        static_cast<size_t>(payloadLength));
}

int TelemetryUploader::buildPayload(
    char *payload,
    size_t payloadSize) {
    if (telemetryService_ != NULL) {
        return telemetryService_->buildPayload(payload, payloadSize);
    }

    return payloadBuildFunction_(payload, payloadSize);
}