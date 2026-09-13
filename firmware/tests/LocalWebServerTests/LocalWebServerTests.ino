#include <utility>
#include <Arduino.h>
#include <string.h>
#include <type_traits>
#include "lwip/opt.h"
#include "lwip/arch.h"

#include "src/config/AppConfig.h"
#include "src/http/LocalWebServer.h"
#include "src/http/Az3166LocalWebServerOperations.h"
#include "src/telemetry/TelemetryHttpHandler.h"
#include "src/telemetry/TelemetryService.h"

struct FakeHttpPlatform;
class FakeLocalWebServerOperations;

static_assert(std::is_abstract<LocalWebServerOperations>::value,
              "LocalWebServerOperations must remain an interface");
static_assert(std::has_virtual_destructor<LocalWebServerOperations>::value,
              "LocalWebServerOperations must have a virtual destructor");
static_assert(!std::is_copy_constructible<LocalHttpSocket>::value &&
                  !std::is_copy_assignable<LocalHttpSocket>::value,
              "LocalHttpSocket must not duplicate descriptor ownership");
static_assert(std::is_nothrow_move_constructible<LocalHttpSocket>::value &&
                  std::is_nothrow_move_assignable<LocalHttpSocket>::value,
              "LocalHttpSocket must transfer ownership without throwing");

int failureCount = 0;

const int TRANSIENT_ACCEPT_ERRORS[] = {
    LWIP_EAGAIN, LWIP_EWOULDBLOCK, LWIP_EINTR, LWIP_ECONNABORTED, LWIP_ECONNRESET
};

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

void testAcceptErrorClassification() {
    bool transientClassified = true;
    for (int socketError : TRANSIENT_ACCEPT_ERRORS) {
        transientClassified &= LocalWebServer::classifyAcceptError(socketError) ==
            LocalWebServer::ACCEPT_IDLE;
    }
    expect(transientClassified,
           "would-block, interrupted, aborted, and reset accepts keep the listener alive");

    const int fatalErrors[] = {LWIP_EBADF, LWIP_ENOTSOCK, LWIP_EINVAL, LWIP_ENETDOWN, 0};
    bool fatalClassified = true;
    for (int socketError : fatalErrors) {
        fatalClassified &= LocalWebServer::classifyAcceptError(socketError) ==
            LocalWebServer::ACCEPT_ERROR;
    }
    expect(fatalClassified,
           "fatal and unrecognized accept errors retain listener recovery");
}

class NativeAcceptOperations : public Az3166LocalWebServerOperations {
public:
    int readyResult = 1;
    int optionResult = 0;
    int socketError = LWIP_EWOULDBLOCK;
    socklen_t returnedSize = sizeof(int);
    bool writeError = true;
    bool lookupContract = true;
    int readyCount = 0;
    int acceptCount = 0;
    int lookupCount = 0;
    int closeCount = 0;

    void closeSocket(int) override { ++closeCount; }

protected:
    int listenerReady(int listener) override {
        ++readyCount;
        lookupContract &= listener == 10;
        return readyResult;
    }

    int acceptSocket(int listener) override {
        ++acceptCount;
        lookupContract &= listener == 10;
        return -1;
    }

    int getSocketOption(int descriptor, int level, int option,
                        void *value, socklen_t *length) override {
        ++lookupCount;
        if (value == NULL || length == NULL) {
            lookupContract = false;
            return -1;
        }
        lookupContract &= descriptor == 10 && level == SOL_SOCKET &&
            option == SO_ERROR && *length == sizeof(int);
        if (writeError) {
            *static_cast<int *>(value) = socketError;
        }
        *length = returnedSize;
        return optionResult;
    }
};

void testNativeAcceptErrorLookup() {
    NativeAcceptOperations operations;
    bool transientHandled = true;
    int expectedLookups = 0;
    for (int socketError : TRANSIENT_ACCEPT_ERRORS) {
        operations.socketError = socketError;
        transientHandled &= operations.acceptClient(10) == LocalWebServer::ACCEPT_IDLE;
        ++expectedLookups;
    }
    expect(transientHandled, "native accept uses SO_ERROR to preserve transient failures");

    const int fatalErrors[] = {LWIP_EBADF, LWIP_ENOTSOCK, LWIP_EINVAL, LWIP_ENETDOWN, 0};
    bool fatalHandled = true;
    for (int socketError : fatalErrors) {
        operations.socketError = socketError;
        fatalHandled &= operations.acceptClient(10) == LocalWebServer::ACCEPT_ERROR;
        ++expectedLookups;
    }
    expect(fatalHandled, "native accept retains recovery for fatal or unrecognized SO_ERROR");
    expect(operations.lookupContract && operations.readyCount == expectedLookups &&
               operations.acceptCount == expectedLookups &&
               operations.lookupCount == expectedLookups && operations.closeCount == 0,
           "native error lookup uses the listener, SOL_SOCKET, SO_ERROR, and exact int size");
}

void testNativeAcceptInvalidErrorLookup() {
    NativeAcceptOperations operations;
    operations.optionResult = -1;
    expect(operations.acceptClient(10) == LocalWebServer::ACCEPT_ERROR,
           "failed SO_ERROR lookup cannot be accepted as a transient error");

    operations.optionResult = 0;
    const socklen_t invalidSizes[] = {0, sizeof(int) - 1, sizeof(int) + 1};
    bool invalidSizesRejected = true;
    for (socklen_t length : invalidSizes) {
        operations.returnedSize = length;
        invalidSizesRejected &= operations.acceptClient(10) == LocalWebServer::ACCEPT_ERROR;
    }
    expect(invalidSizesRejected, "zero, short, and oversized SO_ERROR lengths are rejected");

    operations.returnedSize = sizeof(int);
    operations.writeError = false;
    expect(operations.acceptClient(10) == LocalWebServer::ACCEPT_ERROR,
           "SO_ERROR lookup without an error value does not reuse a previous transient value");
    expect(operations.lookupContract && operations.lookupCount == 5 && operations.closeCount == 0,
           "failed error lookups never close the listener or an invalid accepted handle");
}

void testNativeAcceptReadinessFailures() {
    NativeAcceptOperations operations;
    operations.readyResult = 0;
    expect(operations.acceptClient(10) == LocalWebServer::ACCEPT_IDLE,
           "native accept skips an idle listener");
    operations.readyResult = -1;
    expect(operations.acceptClient(10) == LocalWebServer::ACCEPT_ERROR,
           "native accept reports a failed readiness check");
    expect(operations.readyCount == 2 && operations.acceptCount == 0 &&
               operations.lookupCount == 0 && operations.closeCount == 0,
           "idle or invalid readiness never calls accept or SO_ERROR");
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
    int lastClosedDescriptor;
    int advertisedCloseCount;
    int clientAcceptCount;
    int acceptSocketError;
    int acceptFailureCount;
    int readyCount;
    int serviceUpdates;
    int serviceWithdrawals;
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

class FakeLocalWebServerOperations : public LocalWebServerOperations {
public:
    FakeLocalWebServerOperations(
        FakeHttpPlatform &state,
        rtos::Mutex &mutex,
        rtos::Semaphore &progressSignal,
        rtos::Semaphore &startupGate)
        : fake(state), fakeMutex(mutex), progress(progressSignal),
          openGate(startupGate) {
    }

    uint32_t currentTime() override;
    int openListener(uint32_t address, uint16_t port) override;
    int acceptClient(int listener) override;
    int receiveBytes(int client, char *buffer, size_t size) override;
    int sendBytes(int client, const char *buffer, size_t size) override;
    void closeSocket(int descriptor) override;
    void updateService(bool available, uint32_t address);

private:
    FakeHttpPlatform &fake;
    rtos::Mutex &fakeMutex;
    rtos::Semaphore &progress;
    rtos::Semaphore &openGate;
};

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

uint32_t FakeLocalWebServerOperations::currentTime() {
    fakeMutex.lock();
    uint32_t now = fake.now;
    fakeMutex.unlock();
    return now;
}

int FakeLocalWebServerOperations::openListener(uint32_t address, uint16_t port) {
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

int FakeLocalWebServerOperations::acceptClient(int) {
    fakeMutex.lock();
    int result = LocalWebServer::ACCEPT_IDLE;
    if (fake.acceptError) {
        result = LocalWebServer::ACCEPT_ERROR;
        fake.acceptError = false;
    } else if (fake.acceptSocketError != 0) {
        result = LocalWebServer::classifyAcceptError(fake.acceptSocketError);
        fake.acceptSocketError = 0;
        ++fake.acceptFailureCount;
    } else if (fake.clientQueued) {
        fake.clientQueued = false;
        ++fake.clientAcceptCount;
        result = 20;
    }
    fakeMutex.unlock();
    progress.release();
    return result;
}

int FakeLocalWebServerOperations::receiveBytes(int, char *buffer, size_t size) {
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

int FakeLocalWebServerOperations::sendBytes(int, const char *buffer, size_t size) {
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

void FakeLocalWebServerOperations::closeSocket(int descriptor) {
    fakeMutex.lock();
    fake.lastClosedDescriptor = descriptor;
    if (descriptor == 20) {
        ++fake.closeClientCount;
    } else {
        ++fake.closeListenerCount;
        if (fake.advertised) {
            ++fake.advertisedCloseCount;
        }
        fake.listenerOpen = false;
    }
    fakeMutex.unlock();
    progress.release();
}

void FakeLocalWebServerOperations::updateService(bool available, uint32_t address) {
    fakeMutex.lock();
    ++fake.serviceUpdates;
    if (available && !fake.listenerOpen) {
        ++fake.invalidAdvertisements;
    }
    if (available && (!fake.advertised || fake.advertisedAddress != address)) {
        ++fake.readyCount;
    }
    if (!available && fake.advertised) {
        ++fake.serviceWithdrawals;
    }
    fake.advertised = available;
    fake.advertisedAddress = address;
    fakeMutex.unlock();
    progress.release();
}

class StartupObservingOperations : public FakeLocalWebServerOperations {
public:
    StartupObservingOperations()
        : FakeLocalWebServerOperations(::fake, ::fakeMutex, ::progress, ::openGate),
          server(NULL),
          workerProgressed(false),
          stateDuringStartup({false, false, 0, 0}),
          callerThread_(osThreadGetId()),
          callerClockCalls_(0) {
    }

    uint32_t currentTime() override {
        uint32_t now = FakeLocalWebServerOperations::currentTime();
        if (osThreadGetId() == callerThread_ && ++callerClockCalls_ == 2) {
            workerProgressed = waitForCount(&FakeHttpPlatform::serviceUpdates, 2);
            if (workerProgressed && server != NULL) {
                stateDuringStartup = server->state();
            }
        }
        return now;
    }

    LocalWebServer *server;
    bool workerProgressed;
    LocalWebServerState stateDuringStartup;

private:
    osThreadId callerThread_;
    int callerClockCalls_;
};

void testServiceCallbackBinding() {
    resetHttpPlatform();
    LocalHttpServiceUpdate empty;
    expect(!empty, "service callbacks are optional and empty by default");

    LocalHttpServiceUpdate original =
        mbed::callback(&httpOperations(), &FakeLocalWebServerOperations::updateService);
    LocalHttpServiceUpdate copied = original;
    original = LocalHttpServiceUpdate();
    expect(!original && copied, "copying retains the bound member callback");

    fakeMutex.lock();
    fake.listenerOpen = true;
    fakeMutex.unlock();
    copied(true, 0xC0000201UL);
    fakeMutex.lock();
    bool announced = fake.advertised && fake.advertisedAddress == 0xC0000201UL &&
        fake.serviceUpdates == 1 && fake.invalidAdvertisements == 0;
    fakeMutex.unlock();
    expect(announced, "member callback receives availability and address on its bound object");

    copied(false, 0);
    fakeMutex.lock();
    bool withdrawn = !fake.advertised && fake.advertisedAddress == 0 &&
        fake.serviceUpdates == 2;
    fakeMutex.unlock();
    expect(withdrawn, "copied callback also forwards the unavailable state");
}

FakeLocalWebServerOperations &httpOperations() {
    static FakeLocalWebServerOperations operations(fake, fakeMutex, progress, openGate);
    return operations;
}

void testSocketScopeAndReset() {
    resetHttpPlatform();
    {
        LocalHttpSocket empty(httpOperations());
        LocalHttpSocket failed(httpOperations(), LocalWebServer::ACCEPT_ERROR);
        expect(!empty && empty.get() == -1 && !failed &&
                   failed.get() == LocalWebServer::ACCEPT_ERROR,
               "empty and failed handles retain their non-owning status");
    }
    expect(fake.closeListenerCount == 0 && fake.closeClientCount == 0,
           "invalid handles are never closed");
    {
        LocalHttpSocket socket(httpOperations(), 0);
        expect(socket && socket.get() == 0, "descriptor zero is a valid owned socket");
        socket.reset(0);
        expect(fake.closeListenerCount == 0, "resetting to the same descriptor does not close it");
        socket.reset(10);
        expect(socket.get() == 10 && fake.closeListenerCount == 1 &&
                   fake.lastClosedDescriptor == 0,
               "reset closes the previous descriptor before replacing ownership");
        socket.reset();
        socket.reset();
        expect(!socket && fake.closeListenerCount == 2 && fake.lastClosedDescriptor == 10,
               "repeated reset closes an owned socket exactly once");
    }
    expect(fake.closeListenerCount == 2, "destruction does not close an already reset socket");
    {
        LocalHttpSocket socket(httpOperations(), 20);
    }
    expect(fake.closeClientCount == 1 && fake.lastClosedDescriptor == 20,
           "scope exit closes the remaining owned socket exactly once");
}

void testSocketRelease() {
    resetHttpPlatform();
    int descriptor;
    {
        LocalHttpSocket socket(httpOperations(), 20);
        descriptor = socket.release();
        expect(descriptor == 20 && !socket && socket.release() == -1,
               "release transfers the raw descriptor and leaves the owner empty");
    }
    expect(fake.closeClientCount == 0 && fake.closeListenerCount == 0,
           "destroying a released owner does not close the transferred descriptor");
    {
        LocalHttpSocket receiver(httpOperations(), descriptor);
    }
    expect(fake.closeClientCount == 1 && fake.lastClosedDescriptor == descriptor,
           "the receiving owner closes a released descriptor once");
}

void testSocketMoveOwnership() {
    resetHttpPlatform();
    {
        LocalHttpSocket source(httpOperations(), 20);
        LocalHttpSocket destination(std::move(source));
        expect(!source && destination.get() == 20 && fake.closeClientCount == 0,
               "move construction transfers ownership without closing the socket");
        destination = std::move(destination);
        expect(destination.get() == 20 && fake.closeClientCount == 0,
               "self move preserves socket ownership");
    }
    expect(fake.closeClientCount == 1, "moved-from destruction cannot double-close a socket");

    resetHttpPlatform();
    static FakeHttpPlatform secondFake;
    memset(&secondFake, 0, sizeof(secondFake));
    rtos::Mutex secondMutex;
    rtos::Semaphore secondProgress(0);
    rtos::Semaphore secondOpenGate(0);
    FakeLocalWebServerOperations secondOperations(
        secondFake, secondMutex, secondProgress, secondOpenGate);
    {
        LocalHttpSocket source(secondOperations, 10);
        LocalHttpSocket destination(httpOperations(), 10);
        destination = std::move(source);
        expect(!source && destination.get() == 10 && fake.closeListenerCount == 1 &&
                   fake.lastClosedDescriptor == 10 && secondFake.closeListenerCount == 0,
               "move assignment closes the target through its original backend");
        source.reset(20);
        expect(source.get() == 20 && secondFake.closeClientCount == 0,
               "a moved-from socket can adopt another descriptor on its original backend");
    }
    expect(fake.closeListenerCount == 1 && secondFake.closeListenerCount == 1 &&
               secondFake.closeClientCount == 1,
           "transferred and reused owners close only through their correct backends");

    {
        LocalHttpSocket empty(secondOperations);
        LocalHttpSocket destination(httpOperations(), 10);
        destination = std::move(empty);
        expect(!destination && fake.closeListenerCount == 2,
               "moving an empty owner closes the destination and leaves it empty");
    }
    expect(secondFake.closeListenerCount == 1 && secondFake.closeClientCount == 1,
           "empty moved owners perform no extra cleanup");
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

void testWorkerProgressDuringStartup() {
    resetHttpPlatform();
    StartupObservingOperations operations;
    FakeLocalWebServerOperations &callbackOperations = operations;
    ExampleHandler handler;
    LocalWebServer server(handler, 8080, 5000, operations,
        mbed::callback(&callbackOperations, &FakeLocalWebServerOperations::updateService));
    operations.server = &server;

    server.update(true, 0xC0000201UL);
    expect(operations.workerProgressed,
           "the worker can publish listener readiness before startup completion returns");
    expect(operations.stateDuringStartup.workerStarted &&
               operations.stateDuringStartup.listening &&
               operations.stateDuringStartup.address == 0xC0000201UL,
           "early worker progress publishes a consistent started and listening snapshot");
    server.update(true, 0xC0000201UL);
    expect(fakeCount(&FakeHttpPlatform::openCount) == 1 && server.state().error == 0,
           "startup completion retains the worker state without reopening the listener");
}

void testStartupCompletionPreservesListenerError() {
    resetHttpPlatform();
    const int listenerError = -71;
    fake.openResult = listenerError;
    StartupObservingOperations operations;
    FakeLocalWebServerOperations &callbackOperations = operations;
    ExampleHandler handler;
    LocalWebServer server(handler, 8080, 5000, operations,
        mbed::callback(&callbackOperations, &FakeLocalWebServerOperations::updateService));
    operations.server = &server;

    server.update(true, 0xC0000201UL);
    expect(operations.workerProgressed && operations.stateDuringStartup.workerStarted &&
               !operations.stateDuringStartup.listening &&
               operations.stateDuringStartup.error == listenerError,
           "a listener failure can be published while the caller completes startup");
    LocalWebServerState state = server.state();
    expect(state.workerStarted && !state.listening && state.error == listenerError,
           "successful thread startup does not erase an already-published listener error");
}

void testWorkerLifecycle() {
    resetHttpPlatform();
    ExampleHandler handler;
    {
        LocalWebServer server(handler, 8080, 5000, httpOperations(),
                              mbed::callback(&httpOperations(), &FakeLocalWebServerOperations::updateService));
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
    LocalWebServer server(handler, 8080, 5000, httpOperations(),
                          mbed::callback(&httpOperations(), &FakeLocalWebServerOperations::updateService));
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

void testTransientAcceptFailures() {
    resetHttpPlatform();
    ExampleHandler handler;
    LocalWebServer server(handler, 8080, 5000, httpOperations(),
        mbed::callback(&httpOperations(), &FakeLocalWebServerOperations::updateService));
    server.update(true, 0xC0000201UL);
    expect(waitForCount(&FakeHttpPlatform::readyCount, 1),
           "transient accept test starts an advertised listener");

    const char request[] = "GET /example HTTP/1.1\r\n\r\n";
    int expectedClients = 0;
    bool requestsCompleted = true;
    for (int socketError : TRANSIENT_ACCEPT_ERRORS) {
        fakeMutex.lock();
        fake.acceptSocketError = socketError;
        fakeMutex.unlock();
        queueRequest(request, sizeof(request) - 1);
        ++expectedClients;
        requestsCompleted &= waitForCount(&FakeHttpPlatform::closeClientCount, expectedClients);
    }

    LocalWebServerState state = server.state();
    fakeMutex.lock();
    bool listenerPreserved = fake.listenerOpen && fake.advertised &&
        fake.openCount == 1 && fake.closeListenerCount == 0 &&
        fake.readyCount == 1 && fake.serviceWithdrawals == 0;
    bool failuresExercised = fake.acceptFailureCount == expectedClients &&
        fake.clientAcceptCount == expectedClients && fake.closeClientCount == expectedClients &&
        fake.now == 0;
    fakeMutex.unlock();

    expect(requestsCompleted && failuresExercised && outputContains("HTTP/1.1 200 OK"),
           "queued clients succeed after each transient accept without advancing the retry clock");
    expect(listenerPreserved && state.workerStarted && state.listening && state.error == 0,
           "transient accepts neither reopen the listener nor withdraw discovery");
}

void testAddressChangesDuringStartup() {
    resetHttpPlatform();
    fake.holdOpen = true;
    ExampleHandler handler;
    LocalWebServer server(handler, 8080, 5000, httpOperations(),
                          mbed::callback(&httpOperations(), &FakeLocalWebServerOperations::updateService));
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

void testSocketCleanupDuringShutdown() {
    resetHttpPlatform();
    ExampleHandler handler;
    {
        LocalWebServer server(handler, 8080, 5000, httpOperations(),
            mbed::callback(&httpOperations(), &FakeLocalWebServerOperations::updateService));
        server.update(true, 0xC0000201UL);
        const char incomplete[] = "GET /example HTTP/1.1\r\n";
        queueRequest(incomplete, sizeof(incomplete) - 1);
        expect(waitForCount(&FakeHttpPlatform::clientAcceptCount, 1),
               "shutdown test holds an accepted client with an incomplete request");
    }
    expect(fake.closeClientCount == 1 && fake.closeListenerCount == 1 &&
               fake.lastClosedDescriptor == 10,
           "destruction closes the in-progress client and then its listener exactly once");
    expect(!fake.advertised && fake.advertisedCloseCount == 0 && fake.outputLength == 0,
           "shutdown withdraws discovery before listener closure without sending a stale response");
}

void testIndependentOperations() {
    resetHttpPlatform();
    static FakeHttpPlatform secondFake;
    memset(&secondFake, 0, sizeof(secondFake));
    secondFake.openResult = 10;
    secondFake.now = 200;
    rtos::Mutex secondMutex;
    rtos::Semaphore secondProgress(0);
    rtos::Semaphore secondOpenGate(0);
    FakeLocalWebServerOperations secondOperations(
        secondFake, secondMutex, secondProgress, secondOpenGate);
    ExampleHandler handler;
    LocalWebServer firstServer(handler, 8080, 5000, httpOperations(),
        mbed::callback(&httpOperations(), &FakeLocalWebServerOperations::updateService));
    LocalWebServer secondServer(handler, 8081, 5000, secondOperations,
        mbed::callback(&secondOperations, &FakeLocalWebServerOperations::updateService));
    firstServer.update(true, 0xC0000201UL);
    secondServer.update(true, 0xC0000202UL);
    expect(waitForCount(&FakeHttpPlatform::readyCount, 1),
           "first HTTP worker starts through its injected interface");

    uint32_t started = millis();
    bool secondReady = false;
    do {
        secondMutex.lock();
        secondReady = secondFake.advertised;
        secondMutex.unlock();
        if (secondReady) {
            break;
        }
        secondProgress.wait(20);
    } while (millis() - started < 1000UL);
    expect(secondReady && firstServer.state().address == 0xC0000201UL &&
               secondServer.state().address == 0xC0000202UL,
           "independent backend instances keep worker addresses separate");
    expect(httpOperations().currentTime() == 0 && secondOperations.currentTime() == 200,
           "HTTP backends have independent clocks");

    firstServer.update(false, 0);
    expect(waitForCount(&FakeHttpPlatform::closeListenerCount, 1),
           "first worker shuts down its own listener");
    secondMutex.lock();
    bool isolated = secondFake.listenerOpen && secondFake.advertised &&
        secondFake.lastPort == 8081 && secondFake.openCount == 1 &&
        secondFake.closeListenerCount == 0 && secondFake.invalidAdvertisements == 0;
    secondMutex.unlock();
    expect(isolated, "stopping one HTTP backend leaves the other listener unchanged");
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
    testAcceptErrorClassification();
    testNativeAcceptErrorLookup();
    testNativeAcceptInvalidErrorLookup();
    testNativeAcceptReadinessFailures();
    testDisconnectedPolling();
    testTelemetryHandler();
    testSocketScopeAndReset();
    testSocketRelease();
    testSocketMoveOwnership();
    testIndependentOperations();
    testServiceCallbackBinding();
    testWorkerProgressDuringStartup();
    testStartupCompletionPreservesListenerError();
    testWorkerLifecycle();
    testListenerFailureAndRetry();
    testTransientAcceptFailures();
    testAddressChangesDuringStartup();
    testWorkerRequests();
    testRequestBoundsAndDisconnect();
    testResponseDeadline();
    testSocketCleanupDuringShutdown();
}

void loop() {
    Serial.print("TEST_RESULT: ");
    Serial.println(failureCount == 0 ? "PASS" : "FAIL");
    delay(1000);
}