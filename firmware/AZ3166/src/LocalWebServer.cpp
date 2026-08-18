#include "LocalWebServer.h"

#include "AppConfig.h"
#include "TelemetryService.h"

namespace {

const unsigned long REQUEST_TIMEOUT_MS = 2000UL;
const size_t REQUEST_LINE_SIZE = 96;

}  // namespace

LocalWebServer::LocalWebServer(
        TelemetryService &telemetryService,
    uint16_t port,
    uint32_t startRetryIntervalMs)
        : telemetryService_(telemetryService),
            server_(port),
      startRetryIntervalMs_(startRetryIntervalMs),
      lastStartAttempt_(0),
      wasWiFiConnected_(false) {
}

void LocalWebServer::poll(bool wifiConnected) {
    if (!wifiConnected) {
        if (wasWiFiConnected_) {
            server_.close();
        }
        wasWiFiConnected_ = false;
        lastStartAttempt_ = 0;
        return;
    }

    uint32_t now = millis();
    bool newlyConnected = !wasWiFiConnected_;
    wasWiFiConnected_ = true;
    if (newlyConnected ||
        now - lastStartAttempt_ >= startRetryIntervalMs_) {
        // Core begin() returns no status, so retry silent failures at a bounded rate.
        server_.begin();
        server_.setTimeout(0);
        lastStartAttempt_ = now;
    }

    WiFiClient client = server_.available();
    if (!client) {
        return;
    }

    char requestLine[REQUEST_LINE_SIZE];
    if (!readRequest(client, requestLine, sizeof(requestLine))) {
        sendResponse(
            client,
            "400 Bad Request",
            "{\"error\":\"bad request\"}");
        Serial.println("Local HTTP response: 400 Bad Request");
    } else if (routeRequest(requestLine) == LOCAL_ROUTE_TELEMETRY) {
        sendTelemetryResponse(client);
    } else {
        sendResponse(
            client,
            "404 Not Found",
            "{\"error\":\"not found\"}");
        Serial.println("Local HTTP response: 404 Not Found");
    }

    delay(1);
    client.stop();
}

LocalRequestRoute LocalWebServer::routeRequest(
    const char *requestLine) {
    if (requestLine != NULL &&
        strncmp(
            requestLine,
            AppConfig::TELEMETRY_REQUEST,
            sizeof(AppConfig::TELEMETRY_REQUEST) - 1) == 0) {
        return LOCAL_ROUTE_TELEMETRY;
    }

    return LOCAL_ROUTE_NOT_FOUND;
}

bool LocalWebServer::readRequest(
    WiFiClient &client,
    char *requestLine,
    size_t requestLineSize) {
    unsigned long requestStart = millis();
    size_t requestLength = 0;
    bool readingRequestLine = true;
    bool currentLineIsBlank = true;

    requestLine[0] = '\0';

    while (client.connected() &&
           millis() - requestStart < REQUEST_TIMEOUT_MS) {
        if (!client.available()) {
            delay(1);
            continue;
        }

        int value = client.read();
        if (value < 0) {
            delay(1);
            continue;
        }

        char current = static_cast<char>(value);
        if (readingRequestLine) {
            if (current == '\n') {
                requestLine[requestLength] = '\0';
                readingRequestLine = false;
            } else if (current != '\r' &&
                       requestLength + 1 < requestLineSize) {
                requestLine[requestLength++] = current;
            }
        }

        if (current == '\n' && currentLineIsBlank) {
            return requestLength > 0;
        }

        if (current == '\n') {
            currentLineIsBlank = true;
        } else if (current != '\r') {
            currentLineIsBlank = false;
        }
    }

    return false;
}

void LocalWebServer::sendTelemetryResponse(WiFiClient &client) {
    char body[AppConfig::TELEMETRY_PAYLOAD_SIZE];
    int bodyLength = telemetryService_.buildPayload(
        body,
        sizeof(body));
    if (bodyLength == TelemetryService::PAYLOAD_SENSOR_ERROR) {
        sendResponse(
            client,
            "503 Service Unavailable",
            "{\"error\":\"sensor read failed\"}");
        Serial.println("Local HTTP response: 503 Service Unavailable");
        return;
    }
    if (bodyLength == TelemetryService::PAYLOAD_FORMAT_ERROR) {
        sendResponse(
            client,
            "500 Internal Server Error",
            "{\"error\":\"payload formatting failed\"}");
        Serial.println("Local HTTP response: 500 Internal Server Error");
        return;
    }

    sendResponse(client, "200 OK", body);
    Serial.print("Local telemetry JSON: ");
    Serial.println(body);
}

void LocalWebServer::sendResponse(
    WiFiClient &client,
    const char *status,
    const char *body) {
    sendHeader(client, status);
    sendBody(client, body);
}

void LocalWebServer::sendHeader(
    WiFiClient &client,
    const char *status) {
    client.print("HTTP/1.1 ");
    client.println(status);
    client.println("Content-Type: application/json");
    client.println("Connection: close");
    client.println();
}

void LocalWebServer::sendBody(
    WiFiClient &client,
    const char *body) {
    client.println(body);
}