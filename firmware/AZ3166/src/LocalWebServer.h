#ifndef LOCAL_WEB_SERVER_H
#define LOCAL_WEB_SERVER_H

#include <AZ3166WiFiServer.h>

enum LocalRequestRoute {
    LOCAL_ROUTE_TELEMETRY,
    LOCAL_ROUTE_NOT_FOUND
};

class TelemetryService;

class LocalWebServer {
public:
    LocalWebServer(
        TelemetryService &telemetryService,
        uint16_t port,
        uint32_t startRetryIntervalMs);

    void poll(bool wifiConnected);

    static LocalRequestRoute routeRequest(const char *requestLine);

private:
    bool readRequest(
        WiFiClient &client,
        char *requestLine,
        size_t requestLineSize);
    void sendTelemetryResponse(WiFiClient &client);
    void sendResponse(
        WiFiClient &client,
        const char *status,
        const char *body);
    void sendHeader(WiFiClient &client, const char *status);
    void sendBody(WiFiClient &client, const char *body);

    TelemetryService &telemetryService_;
    WiFiServer server_;
    uint32_t startRetryIntervalMs_;
    uint32_t lastStartAttempt_;
    bool wasWiFiConnected_;
};

#endif