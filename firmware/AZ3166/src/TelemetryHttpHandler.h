#ifndef TELEMETRY_HTTP_HANDLER_H
#define TELEMETRY_HTTP_HANDLER_H

#include "LocalHttpHandler.h"

class TelemetryService;

enum LocalRequestRoute {
    LOCAL_ROUTE_TELEMETRY,
    LOCAL_ROUTE_NOT_FOUND
};

typedef int (*TelemetryHttpPayloadBuilder)(char *, size_t, void *);

class TelemetryHttpHandler : public LocalHttpHandler {
public:
    explicit TelemetryHttpHandler(TelemetryService &telemetryService);
    TelemetryHttpHandler(TelemetryHttpPayloadBuilder builder, void *context);

    LocalHttpResponse handle(
        const char *requestLine,
        char *body,
        size_t bodySize) override;

    static LocalRequestRoute routeRequest(const char *requestLine);

private:
    TelemetryHttpPayloadBuilder builder_;
    void *context_;
};

#endif