#include <Arduino.h>
#include <string.h>

#include "AppConfig.h"
#include "LocalWebServer.h"
#include "TelemetryHttpHandler.h"
#include "TelemetryService.h"

struct FakeHttpPlatform;

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
        TelemetryHttpHandler::routeRequest(
            "GET /api/telemetry HTTP/1.1") == LOCAL_ROUTE_TELEMETRY,
        "HTTP 1.1 telemetry request selects the telemetry route");
    expect(
        TelemetryHttpHandler::routeRequest(
            "GET /api/telemetry HTTP/1.0") == LOCAL_ROUTE_TELEMETRY,
        "HTTP 1.0 telemetry request selects the telemetry route");
}

void testUnknownRoutes() {
    expect(
        TelemetryHttpHandler::routeRequest(
            "POST /api/telemetry HTTP/1.1") == LOCAL_ROUTE_NOT_FOUND,
        "POST telemetry request is rejected");
    expect(
        TelemetryHttpHandler::routeRequest(
            "GET /api/telemetry/extra HTTP/1.1") == LOCAL_ROUTE_NOT_FOUND,
        "telemetry subpath is rejected");
    expect(
        TelemetryHttpHandler::routeRequest(
            "GET /api/telemetry?fresh=true HTTP/1.1") ==
            LOCAL_ROUTE_NOT_FOUND,
        "telemetry query string is rejected");
    expect(
        TelemetryHttpHandler::routeRequest(
            "GET /api/unknown HTTP/1.1") == LOCAL_ROUTE_NOT_FOUND,
        "unknown path is rejected");
    expect(
        TelemetryHttpHandler::routeRequest(
            "GET /api/telemetry") == LOCAL_ROUTE_NOT_FOUND,
        "request without HTTP version is rejected");
    expect(
        TelemetryHttpHandler::routeRequest("") == LOCAL_ROUTE_NOT_FOUND,
        "empty request line is rejected");
    expect(
        TelemetryHttpHandler::routeRequest(NULL) == LOCAL_ROUTE_NOT_FOUND,
        "null request line is rejected");
}

void testDisconnectedPolling() {
    TelemetryService telemetryService;
    TelemetryHttpHandler handler(telemetryService);
    LocalWebServer server(handler, 8080, 5000);

        server.update(false, 0);
        server.update(false, 0);
        expect(!server.state().workerStarted && !server.state().listening,
            "disconnected updates do not create a worker or listener");
}

int payloadResult = 0;
int payloadBuildCount = 0;

int buildFakePayload(char *body, size_t bodySize, void *context) {
    ++payloadBuildCount;
    if (context != &payloadResult) {
        return TelemetryService::PAYLOAD_FORMAT_ERROR;
    }
    if (payloadResult > 0 && bodySize >= 3) {
        strcpy(body, "{}");
    }
    return payloadResult;
}

void testTelemetryHandler() {
    TelemetryHttpHandler handler(buildFakePayload, &payloadResult);
    char body[160];
    payloadBuildCount = 0;
    payloadResult = 2;
    LocalHttpResponse response = handler.handle("GET /other HTTP/1.1", body, sizeof(body));
    expect(strcmp(response.status, "404 Not Found") == 0 && payloadBuildCount == 0,
           "unknown routes do not read sensors");
    response = handler.handle("GET /api/telemetry HTTP/1.1", body, sizeof(body));
    expect(strcmp(response.status, "200 OK") == 0 &&
               strcmp(response.contentType, "application/json") == 0 &&
               response.bodyLength == 2 && strcmp(body, "{}") == 0,
           "telemetry handler returns the exact payload and JSON content type");
    payloadResult = TelemetryService::PAYLOAD_SENSOR_ERROR;
    response = handler.handle("GET /api/telemetry HTTP/1.1", body, sizeof(body));
    expect(strcmp(response.status, "503 Service Unavailable") == 0 &&
               strcmp(body, "{\"error\":\"sensor read failed\"}") == 0,
           "sensor failures preserve the telemetry 503 response");
    payloadResult = TelemetryService::PAYLOAD_FORMAT_ERROR;
    response = handler.handle("GET /api/telemetry HTTP/1.1", body, sizeof(body));
    expect(strcmp(response.status, "500 Internal Server Error") == 0 &&
               strcmp(body, "{\"error\":\"payload formatting failed\"}") == 0,
           "formatting failures preserve the telemetry 500 response");
    payloadResult = sizeof(body);
    response = handler.handle("GET /api/telemetry HTTP/1.1", body, sizeof(body));
    expect(strcmp(response.status, "500 Internal Server Error") == 0,
           "oversized payload results cannot be sent as successful telemetry");
    TelemetryHttpHandler missing(NULL, NULL);
    response = missing.handle("GET /api/telemetry HTTP/1.1", body, sizeof(body));
    expect(strcmp(response.status, "500 Internal Server Error") == 0,
           "missing payload builders fail safely");
    response = handler.handle("GET /api/telemetry HTTP/1.1", NULL, 0);
    expect(response.bodyLength == 0, "missing response buffers fail safely");
    char tiny[2] = {'x', '!'};
    response = handler.handle("GET /other HTTP/1.1", tiny, 1);
    expect(response.bodyLength == 0 && tiny[0] == '\0' && tiny[1] == '!',
           "small error-response buffers are not overwritten");
}

struct FakeHttpPlatform {
    uint32_t now;
    uint32_t openDuration;
    uint32_t lastAddress;
    uint32_t advertisedAddress;
    uint16_t lastPort;
    int openResult;
    int openCount;
    int closeListenerCount;
    int closeClientCount;
    int clientAcceptCount;
    int readyCount;
    int serviceUpdates;
    int invalidAdvertisements;
    int handlerCount;
    int handlerMode;
    int receiveStep;
    int sendStep;
    bool listenerOpen;
    bool advertised;
    bool clientQueued;
    bool acceptError;
    bool holdOpen;
    bool blockSend;
    size_t inputLength;
    size_t inputOffset;
    size_t outputLength;
    osThreadId handlerThread;
    char input[2300];
    char output[1024];
};

FakeHttpPlatform fake = {};
rtos::Mutex fakeMutex;
rtos::Semaphore progress(0);
rtos::Semaphore openGate(0);

void resetHttpPlatform() {
    fakeMutex.lock();
    memset(&fake, 0, sizeof(fake));
    fake.openResult = 10;
    fakeMutex.unlock();
    while (progress.wait(0) > 0) {}
    while (openGate.wait(0) > 0) {}
}

int fakeCount(int FakeHttpPlatform::*counter) {
    fakeMutex.lock();
    int value = fake.*counter;
    fakeMutex.unlock();
    return value;
}

bool waitForCount(int FakeHttpPlatform::*counter, int expected) {
    uint32_t started = millis();
    do {
        if (fakeCount(counter) >= expected) {
            return true;
        }
        progress.wait(20);
    } while (millis() - started < 1000UL);
    return fakeCount(counter) >= expected;
}

uint32_t fakeCurrentTime() {
    fakeMutex.lock();
    uint32_t now = fake.now;
    fakeMutex.unlock();
    return now;
}

int fakeOpenListener(uint32_t address, uint16_t port) {
    fakeMutex.lock();
    ++fake.openCount;
    fake.lastAddress = address;
    fake.lastPort = port;
    bool hold = fake.holdOpen;
    fake.holdOpen = false;
    fakeMutex.unlock();
    progress.release();
    if (hold) {
        openGate.wait(1000);
    }
    fakeMutex.lock();
    fake.now += fake.openDuration;
    int result = fake.openResult;
    fake.listenerOpen = result >= 0;
    fakeMutex.unlock();
    return result;
}

int fakeAcceptClient(int) {
    fakeMutex.lock();
    int result = LocalWebServer::ACCEPT_IDLE;
    if (fake.acceptError) {
        result = LocalWebServer::ACCEPT_ERROR;
        fake.acceptError = false;
    } else if (fake.clientQueued) {
        fake.clientQueued = false;
        ++fake.clientAcceptCount;
        result = 20;
    }
    fakeMutex.unlock();
    progress.release();
    return result;
}

int fakeReceiveBytes(int, char *buffer, size_t size) {
    fakeMutex.lock();
    size_t remaining = fake.inputLength - fake.inputOffset;
    size_t received = remaining < size ? remaining : size;
    if (received > 7) {
        received = 7;
    }
    memcpy(buffer, fake.input + fake.inputOffset, received);
    fake.inputOffset += received;
    if (received == 0) {
        fake.now += fake.receiveStep;
    }
    fakeMutex.unlock();
    return static_cast<int>(received);
}

int fakeSendBytes(int, const char *buffer, size_t size) {
    fakeMutex.lock();
    if (fake.blockSend) {
        fake.now += fake.sendStep;
        fakeMutex.unlock();
        return 0;
    }
    size_t sent = size < 5 ? size : 5;
    if (sent >= sizeof(fake.output) - fake.outputLength) {
        fakeMutex.unlock();
        return -1;
    }
    memcpy(fake.output + fake.outputLength, buffer, sent);
    fake.outputLength += sent;
    fake.output[fake.outputLength] = '\0';
    fakeMutex.unlock();
    return static_cast<int>(sent);
}

void fakeCloseSocket(int descriptor) {
    fakeMutex.lock();
    if (descriptor == 20) {
        ++fake.closeClientCount;
    } else {
        ++fake.closeListenerCount;
        fake.listenerOpen = false;
    }
    fakeMutex.unlock();
    progress.release();
}

void updateFakeService(bool available, uint32_t address, void *context) {
    fakeMutex.lock();
    ++fake.serviceUpdates;
    if (available && (!fake.listenerOpen || context != &fake)) {
        ++fake.invalidAdvertisements;
    }
    if (available && (!fake.advertised || fake.advertisedAddress != address)) {
        ++fake.readyCount;
    }
    fake.advertised = available;
    fake.advertisedAddress = address;
    fakeMutex.unlock();
    progress.release();
}

LocalWebServerOperations httpOperations() {
    LocalWebServerOperations operations = {
        fakeCurrentTime, fakeOpenListener, fakeAcceptClient,
        fakeReceiveBytes, fakeSendBytes, fakeCloseSocket
    };
    return operations;
}

class ExampleHandler : public LocalHttpHandler {
public:
    LocalHttpResponse handle(const char *, char *body, size_t size) override {
        fakeMutex.lock();
        ++fake.handlerCount;
        fake.handlerThread = osThreadGetId();
        int mode = fake.handlerMode;
        fakeMutex.unlock();
        if (mode == 1) {
            return {"200 OK", "text/plain", size + 1};
        }
        if (mode == 2) {
            return {NULL, NULL, 0};
        }
        if (mode == 3) {
            body[0] = 'a';
            body[1] = '\0';
            body[2] = 'b';
            return {"200 OK", "application/octet-stream", 3};
        }
        strcpy(body, "example");
        return {"200 OK", "text/plain", 7};
    }
};

void queueRequest(const char *request, size_t length) {
    fakeMutex.lock();
    memcpy(fake.input, request, length);
    fake.inputLength = length;
    fake.inputOffset = fake.outputLength = 0;
    fake.output[0] = '\0';
    fake.clientQueued = true;
    fakeMutex.unlock();
}

bool outputContains(const char *text) {
    fakeMutex.lock();
    bool found = strstr(fake.output, text) != NULL;
    fakeMutex.unlock();
    return found;
}

void testWorkerLifecycle() {
    resetHttpPlatform();
    ExampleHandler handler;
    {
        LocalWebServer server(handler, 8080, 5000, httpOperations(), updateFakeService, &fake);
        server.update(true, 0);
        expect(!server.state().workerStarted, "worker waits for an assigned address");
        server.update(true, 0xC0000201UL);
        expect(waitForCount(&FakeHttpPlatform::readyCount, 1),
               "successful listen triggers advertisement on the worker");
        expect(server.state().listening && server.state().address == 0xC0000201UL,
               "server publishes actual listener readiness and address");
        fakeMutex.lock();
        bool configured = fake.lastPort == 8080 && fake.lastAddress == 0xC0000201UL;
        fakeMutex.unlock();
        expect(configured, "listener uses the caller's port and address");
        int cycles = fakeCount(&FakeHttpPlatform::serviceUpdates);
        server.update(true, 0xC0000201UL);
        expect(waitForCount(&FakeHttpPlatform::serviceUpdates, cycles + 1) &&
                   fakeCount(&FakeHttpPlatform::openCount) == 1,
               "unchanged connectivity does not reopen the listener");
        server.update(true, 0xC0000202UL);
        expect(waitForCount(&FakeHttpPlatform::readyCount, 2) &&
                   fakeCount(&FakeHttpPlatform::closeListenerCount) == 1,
               "address change closes the old listener before advertising the new one");
        server.update(false, 0);
        expect(!server.state().listening, "disconnect clears published readiness immediately");
        server.update(true, 0xC0000202UL);
        expect(waitForCount(&FakeHttpPlatform::readyCount, 3) &&
                   fakeCount(&FakeHttpPlatform::closeListenerCount) == 2,
               "rapid same-address reconnect still creates a fresh listener");
    }
    expect(fakeCount(&FakeHttpPlatform::closeListenerCount) == 3,
           "destruction joins the worker and closes its listener");
    fakeMutex.lock();
    bool withdrawn = !fake.advertised && fake.invalidAdvertisements == 0;
    fakeMutex.unlock();
    expect(withdrawn, "advertisement never precedes listen and is withdrawn on shutdown");
}

void testListenerFailureAndRetry() {
    resetHttpPlatform();
    fake.openResult = -1;
    fake.openDuration = 100;
    fake.now = 0xFFFFFF00UL;
    ExampleHandler handler;
    LocalWebServer server(handler, 8080, 5000, httpOperations(), updateFakeService, &fake);
    server.update(true, 0xC0000201UL);
    expect(waitForCount(&FakeHttpPlatform::serviceUpdates, 2) &&
               !server.state().listening && fakeCount(&FakeHttpPlatform::readyCount) == 0,
           "failed listener start is never advertised");
    fakeMutex.lock();
    fake.now = 0xFFFFFF00UL + 100UL + 4999UL;
    int cycles = fake.serviceUpdates;
    fakeMutex.unlock();
    waitForCount(&FakeHttpPlatform::serviceUpdates, cycles + 1);
    expect(fakeCount(&FakeHttpPlatform::openCount) == 1,
           "listener retry waits from completion across millis wraparound");
    fakeMutex.lock();
    fake.now = 0xFFFFFF00UL + 100UL + 5000UL;
    fake.openResult = 10;
    fakeMutex.unlock();
    expect(waitForCount(&FakeHttpPlatform::readyCount, 1),
           "listener retry succeeds at the configured deadline");
    fakeMutex.lock();
    fake.acceptError = true;
    fakeMutex.unlock();
    expect(waitForCount(&FakeHttpPlatform::closeListenerCount, 1) &&
               !server.state().listening,
           "listener accept failure clears readiness and releases the socket");
}

void testAddressChangesDuringStartup() {
    resetHttpPlatform();
    fake.holdOpen = true;
    ExampleHandler handler;
    LocalWebServer server(handler, 8080, 5000, httpOperations(), updateFakeService, &fake);
    server.update(true, 0xC0000201UL);
    expect(waitForCount(&FakeHttpPlatform::openCount, 1), "worker starts the listener independently");
    server.update(true, 0xC0000202UL);
    openGate.release();
    expect(waitForCount(&FakeHttpPlatform::readyCount, 1),
           "replacement address is advertised after stale startup is discarded");
    fakeMutex.lock();
    bool current = fake.openCount == 2 && fake.closeListenerCount == 1 &&
        fake.advertisedAddress == 0xC0000202UL && fake.readyCount == 1;
    fakeMutex.unlock();
    expect(current, "stale listener completion is closed without advertisement");
}

void testWorkerRequests() {
    resetHttpPlatform();
    ExampleHandler handler;
    LocalWebServer server(handler, 8080, 5000, httpOperations());
    server.update(true, 0xC0000201UL);
    const char request[] = "GET /example HTTP/1.1\r\nHost: example\r\n\r\n";
    queueRequest(request, sizeof(request) - 1);
    expect(waitForCount(&FakeHttpPlatform::closeClientCount, 1),
           "HTTP completes while the caller waits without polling the server");
    fakeMutex.lock();
    bool differentThread = fake.handlerThread != NULL && fake.handlerThread != osThreadGetId();
    fakeMutex.unlock();
    expect(differentThread, "application handler executes on the dedicated HTTP worker");
    expect(outputContains("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n") &&
               outputContains("Content-Length: 7\r\n") && outputContains("\r\n\r\nexample"),
           "custom handler response survives partial reads and writes");

    fakeMutex.lock();
    fake.handlerMode = 1;
    fakeMutex.unlock();
    queueRequest(request, sizeof(request) - 1);
    expect(waitForCount(&FakeHttpPlatform::closeClientCount, 2) &&
               outputContains("HTTP/1.1 500 Internal Server Error"),
           "oversized handler responses are replaced with a bounded error");
    fakeMutex.lock();
    fake.handlerMode = 2;
    fakeMutex.unlock();
    queueRequest(request, sizeof(request) - 1);
    expect(waitForCount(&FakeHttpPlatform::closeClientCount, 3) &&
               outputContains("HTTP/1.1 500 Internal Server Error"),
           "null response metadata is rejected safely");
    fakeMutex.lock();
    fake.handlerMode = 3;
    fakeMutex.unlock();
    queueRequest(request, sizeof(request) - 1);
    expect(waitForCount(&FakeHttpPlatform::closeClientCount, 4) &&
               outputContains("Content-Length: 3\r\n"),
           "binary response length does not depend on NUL termination");
    fakeMutex.lock();
    bool binary = fake.outputLength >= 3 &&
        memcmp(fake.output + fake.outputLength - 3, "a\0b", 3) == 0;
    fakeMutex.unlock();
    expect(binary, "binary response bytes are preserved");
}

void testRequestBoundsAndDisconnect() {
    resetHttpPlatform();
    ExampleHandler handler;
    LocalWebServer server(handler, 8080, 5000, httpOperations());
    server.update(true, 0xC0000201UL);
    char longRequest[128];
    memset(longRequest, 'x', sizeof(longRequest));
    queueRequest(longRequest, sizeof(longRequest));
    expect(waitForCount(&FakeHttpPlatform::closeClientCount, 1) &&
               outputContains("400 Bad Request") && fakeCount(&FakeHttpPlatform::handlerCount) == 0,
           "overlong request lines are rejected before application dispatch");
    const char embeddedNul[] = "GET /example\0ignored HTTP/1.1\r\n\r\n";
    queueRequest(embeddedNul, sizeof(embeddedNul) - 1);
    expect(waitForCount(&FakeHttpPlatform::closeClientCount, 2) &&
               outputContains("400 Bad Request"),
           "embedded NUL bytes are rejected");
    fakeMutex.lock();
    const char prefix[] = "GET /example HTTP/1.1\r\nX: ";
    memcpy(fake.input, prefix, sizeof(prefix) - 1);
    memset(fake.input + sizeof(prefix) - 1, 'x', sizeof(fake.input) - sizeof(prefix));
    fake.inputLength = sizeof(fake.input) - 1;
    fake.inputOffset = fake.outputLength = 0;
    fake.output[0] = '\0';
    fake.clientQueued = true;
    fakeMutex.unlock();
    expect(waitForCount(&FakeHttpPlatform::closeClientCount, 3) &&
               outputContains("400 Bad Request"),
           "total request header size is bounded");
    const char incomplete[] = "GET /example HTTP/1.1\r\n";
    fakeMutex.lock();
    fake.now = 0xFFFFFF00UL;
    fake.receiveStep = 1000;
    fakeMutex.unlock();
    queueRequest(incomplete, sizeof(incomplete) - 1);
    expect(waitForCount(&FakeHttpPlatform::closeClientCount, 4) &&
               outputContains("400 Bad Request"),
           "incomplete request timeout is wraparound safe");
    fakeMutex.lock();
    fake.receiveStep = 0;
    fakeMutex.unlock();
    queueRequest(incomplete, sizeof(incomplete) - 1);
    expect(waitForCount(&FakeHttpPlatform::clientAcceptCount, 5), "worker accepts the pending slow client");
    server.update(false, 0);
    expect(waitForCount(&FakeHttpPlatform::closeClientCount, 5) &&
               waitForCount(&FakeHttpPlatform::closeListenerCount, 1),
           "disconnect cancels an in-progress client and closes the listener");
    fakeMutex.lock();
    bool noStaleResponse = fake.outputLength == 0;
    fakeMutex.unlock();
    expect(noStaleResponse, "old-session responses are not sent after disconnect");
}

void testResponseDeadline() {
    resetHttpPlatform();
    ExampleHandler handler;
    LocalWebServer server(handler, 8080, 5000, httpOperations());
    server.update(true, 0xC0000201UL);
    fakeMutex.lock();
    fake.blockSend = true;
    fake.sendStep = 1000;
    fakeMutex.unlock();
    const char request[] = "GET /example HTTP/1.1\r\n\r\n";
    queueRequest(request, sizeof(request) - 1);
    expect(waitForCount(&FakeHttpPlatform::closeClientCount, 1),
           "a non-reading client cannot block the HTTP worker indefinitely");
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
    testTelemetryHandler();
    testWorkerLifecycle();
    testListenerFailureAndRetry();
    testAddressChangesDuringStartup();
    testWorkerRequests();
    testRequestBoundsAndDisconnect();
    testResponseDeadline();
}

void loop() {
    Serial.print("TEST_RESULT: ");
    Serial.println(failureCount == 0 ? "PASS" : "FAIL");
    delay(1000);
}