#ifndef CLOUD_TELEMETRY_H
#define CLOUD_TELEMETRY_H

#include <stddef.h>

#include "TelemetryUploadResult.h"

struct CloudTelemetryRequest {
    const char *endpoint;
    const char *rootCertificate;
    const char *apiKeyHeader;
    const char *apiKey;
    const char *contentType;
    const char *accept;
    const char *connection;
    const char *payload;
    size_t payloadLength;
};

struct CloudTelemetryResponse {
    bool received;
    int statusCode;
    int networkError;
    const char *body;
    size_t bodyLength;
};

typedef TelemetryUploadResult (*CloudTelemetryResponseHandler)(
    const CloudTelemetryResponse &response);

struct CloudTelemetryOperations {
    TelemetryUploadResult (*send)(
        const CloudTelemetryRequest &request,
        CloudTelemetryResponseHandler responseHandler);
};

class CloudTelemetry {
public:
    CloudTelemetry(
        const char *endpoint,
        const char *rootCertificate,
        const char *apiKey,
        const char *apiKeyPlaceholder);

    CloudTelemetry(
        const char *endpoint,
        const char *rootCertificate,
        const char *apiKey,
        const char *apiKeyPlaceholder,
        const CloudTelemetryOperations &operations);

    void begin() const;
    bool isConfigured() const;
    TelemetryUploadResult upload(
        const char *payload,
        size_t payloadLength);
    const char *endpoint() const;

    static bool isApiKeyConfigured(
        const char *apiKey,
        const char *apiKeyPlaceholder);
    static bool isSuccessfulStatus(int statusCode);
    static bool isRetryableStatus(int statusCode);

private:
    static TelemetryUploadResult handleResponse(
        const CloudTelemetryResponse &response);

    const char *endpoint_;
    const char *rootCertificate_;
    const char *apiKey_;
    const char *apiKeyPlaceholder_;
    CloudTelemetryOperations operations_;
};

#endif