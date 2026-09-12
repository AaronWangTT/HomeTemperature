#include "TelemetryHttpHandler.h"

#include <stdio.h>
#include <string.h>

#include "AppConfig.h"
#include "TelemetryService.h"

namespace {

int buildTelemetryPayload(char *body, size_t bodySize, void *context) {
    return static_cast<TelemetryService *>(context)->buildPayload(body, bodySize);
}

LocalHttpResponse errorResponse(
    const char *status,
    const char *message,
    char *body,
    size_t bodySize) {
    int length = snprintf(body, bodySize, "%s", message);
    if (length < 0 || static_cast<size_t>(length) >= bodySize) {
        body[0] = '\0';
        return {"500 Internal Server Error", "application/json", 0};
    }
    return {status, "application/json", static_cast<size_t>(length)};
}

}

TelemetryHttpHandler::TelemetryHttpHandler(TelemetryService &telemetryService)
    : TelemetryHttpHandler(buildTelemetryPayload, &telemetryService) {
}

TelemetryHttpHandler::TelemetryHttpHandler(
    TelemetryHttpPayloadBuilder builder,
    void *context)
    : builder_(builder), context_(context) {
}

LocalRequestRoute TelemetryHttpHandler::routeRequest(const char *requestLine) {
    if (requestLine != NULL &&
        strncmp(requestLine, AppConfig::TELEMETRY_REQUEST,
                sizeof(AppConfig::TELEMETRY_REQUEST) - 1) == 0) {
        return LOCAL_ROUTE_TELEMETRY;
    }
    return LOCAL_ROUTE_NOT_FOUND;
}

LocalHttpResponse TelemetryHttpHandler::handle(
    const char *requestLine,
    char *body,
    size_t bodySize) {
    if (body == NULL || bodySize == 0) {
        return {"500 Internal Server Error", "application/json", 0};
    }
    body[0] = '\0';
    if (routeRequest(requestLine) != LOCAL_ROUTE_TELEMETRY) {
        return errorResponse("404 Not Found", "{\"error\":\"not found\"}",
                             body, bodySize);
    }
    int length = builder_ == NULL
        ? TelemetryService::PAYLOAD_FORMAT_ERROR
        : builder_(body, bodySize, context_);
    if (length == TelemetryService::PAYLOAD_SENSOR_ERROR) {
        return errorResponse("503 Service Unavailable",
                             "{\"error\":\"sensor read failed\"}", body, bodySize);
    }
    if (length <= 0 || static_cast<size_t>(length) >= bodySize) {
        return errorResponse("500 Internal Server Error",
                             "{\"error\":\"payload formatting failed\"}",
                             body, bodySize);
    }
    return {"200 OK", "application/json", static_cast<size_t>(length)};
}