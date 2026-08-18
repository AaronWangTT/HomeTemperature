#include <Arduino.h>

#include "AppConfig.h"
#include "LocalWebServer.h"
#include "TelemetryService.h"

int failureCount = 0;

void expect(bool condition, const char *name) {
    Serial.print(condition ? "PASS: " : "FAIL: ");
    Serial.println(name);
    if (!condition) {
        ++failureCount;
    }
}

void testTelemetryRoute() {
    expect(
        LocalWebServer::routeRequest(
            "GET /api/telemetry HTTP/1.1") == LOCAL_ROUTE_TELEMETRY,
        "HTTP 1.1 telemetry request selects the telemetry route");
    expect(
        LocalWebServer::routeRequest(
            "GET /api/telemetry HTTP/1.0") == LOCAL_ROUTE_TELEMETRY,
        "HTTP 1.0 telemetry request selects the telemetry route");
}

void testUnknownRoutes() {
    expect(
        LocalWebServer::routeRequest(
            "POST /api/telemetry HTTP/1.1") == LOCAL_ROUTE_NOT_FOUND,
        "POST telemetry request is rejected");
    expect(
        LocalWebServer::routeRequest(
            "GET /api/telemetry/extra HTTP/1.1") == LOCAL_ROUTE_NOT_FOUND,
        "telemetry subpath is rejected");
    expect(
        LocalWebServer::routeRequest(
            "GET /api/telemetry?fresh=true HTTP/1.1") ==
            LOCAL_ROUTE_NOT_FOUND,
        "telemetry query string is rejected");
    expect(
        LocalWebServer::routeRequest(
            "GET /api/unknown HTTP/1.1") == LOCAL_ROUTE_NOT_FOUND,
        "unknown path is rejected");
    expect(
        LocalWebServer::routeRequest(
            "GET /api/telemetry") == LOCAL_ROUTE_NOT_FOUND,
        "request without HTTP version is rejected");
    expect(
        LocalWebServer::routeRequest("") == LOCAL_ROUTE_NOT_FOUND,
        "empty request line is rejected");
    expect(
        LocalWebServer::routeRequest(NULL) == LOCAL_ROUTE_NOT_FOUND,
        "null request line is rejected");
}

void testDisconnectedPolling() {
    TelemetryService telemetryService;
    LocalWebServer server(telemetryService, 8080, 5000);

    server.poll(false);
    server.poll(false);
    expect(true, "repeated disconnected polling is safe");
}

void setup() {
    Serial.begin(115200);
    while (!Serial);
    delay(3000);

    Serial.println("TEST_SUITE: LocalWebServerTests");
    expect(
        AppConfig::LOCAL_TELEMETRY_PORT == 80,
        "local web server uses port 80");
    expect(
        AppConfig::LOCAL_WEB_SERVER_RETRY_INTERVAL_MS == 5000,
        "local web server retries startup every five seconds");
    testTelemetryRoute();
    testUnknownRoutes();
    testDisconnectedPolling();
}

void loop() {
    Serial.print("TEST_RESULT: ");
    Serial.println(failureCount == 0 ? "PASS" : "FAIL");
    delay(1000);
}