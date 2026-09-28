#include <Arduino.h>
#include <string.h>

#include "src/ota/LocalOtaController.h"
#include "src/ota/LocalOtaHttpHandler.h"
#include "src/connectivity/NetworkMaintenanceCoordinator.h"
#include "src/ota/OtaRebootCoordinator.h"

int failureCount;
uint32_t fakeNow;

void expect(bool condition, const char *name) {
    Serial.print(condition ? "PASS: " : "FAIL: ");
    Serial.println(name);
    if (!condition) {
        ++failureCount;
    }
}

uint32_t readClock() {
    return fakeNow;
}

class FakeCore : public LocalOtaCore {
public:
    int begins = 0;
    int writes = 0;
    int finishes = 0;
    int activates = 0;
    int aborts = 0;
    int errorNames = 0;
    OTAStagingError activationResult = OTA_OK;

    OTAStagingError begin(
        size_t,
        const uint8_t *,
        size_t,
        OTAAdmissionCallback admission,
        OTACancellationCallback cancellation,
        void *context) override {
        ++begins;
        if (cancellation(context)) {
            return OTA_ERROR_CANCELLED;
        }
        OTAStagingMetadata metadata = {};
        strcpy(metadata.productId, "HomeTemperature");
        strcpy(metadata.boardId, "MXCHIP_AZ3166");
        strcpy(metadata.firmwareVersion, "1.0.1");
        return admission(&metadata, context) != 0
            ? OTA_OK : OTA_ERROR_ADMISSION_REJECTED;
    }
    OTAStagingError write(const uint8_t *, size_t) override {
        ++writes;
        return OTA_OK;
    }
    OTAStagingError finish(OTAStagedImageInfo *info) override {
        ++finishes;
        memset(info, 0, sizeof(*info));
        info->sessionGeneration = 7;
        info->sha256[0] = 0x42;
        return OTA_OK;
    }
    OTAStagingError activate(uint32_t generation, const uint8_t digest[32]) override {
        ++activates;
        return generation == 7 && digest[0] == 0x42
            ? activationResult : OTA_ERROR_INVALID_ARGUMENT;
    }
    OTAStagingError abort() override {
        ++aborts;
        return OTA_OK;
    }
    OTAStagingStatus status() override {
        OTAStagingStatus value = {};
        return value;
    }
    const char *errorName(OTAStagingError error) override {
        ++errorNames;
        if (error == OTA_ERROR_WRITE) {
            return "OTA_ERROR_WRITE";
        }
        if (error == OTA_ERROR_SIGNATURE) {
            return "OTA_ERROR_SIGNATURE";
        }
        return error == OTA_OK ? "OTA_OK" : "OTA_ERROR_CANCELLED";
    }
};

class FakeBody : public LocalHttpBodyStream {
public:
    explicit FakeBody(size_t size) : remaining_(size), cancelled_(false) {}
    int reads = 0;
    LocalHttpBodyReadStatus read(
        char *buffer, size_t capacity, size_t &received) override {
        ++reads;
        if (cancelled_) {
            received = 0;
            return LOCAL_HTTP_BODY_CANCELLED;
        }
        if (remaining_ == 0) {
            received = 0;
            return LOCAL_HTTP_BODY_COMPLETE;
        }
        received = remaining_ < capacity ? remaining_ : capacity;
        memset(buffer, 0x5a, received);
        remaining_ -= received;
        return LOCAL_HTTP_BODY_DATA;
    }
    size_t remaining() const override { return remaining_; }
    bool cancelled() const override { return cancelled_; }
    void cancel() { cancelled_ = true; }
protected:
    size_t remaining_;
    bool cancelled_;
};

class IdleBody : public FakeBody {
public:
    explicit IdleBody(size_t size) : FakeBody(size) {}
    LocalHttpBodyReadStatus read(
        char *, size_t, size_t &received) override {
        ++reads;
        received = 0;
        return LOCAL_HTTP_BODY_DATA;
    }
};

class TextBody : public LocalHttpBodyStream {
public:
    explicit TextBody(const char *text)
        : text_(text), offset_(0), length_(strlen(text)) {}
    LocalHttpBodyReadStatus read(
        char *buffer, size_t capacity, size_t &received) override {
        if (offset_ == length_) {
            received = 0;
            return LOCAL_HTTP_BODY_COMPLETE;
        }
        received = length_ - offset_ < capacity
            ? length_ - offset_ : capacity;
        memcpy(buffer, text_ + offset_, received);
        offset_ += received;
        return LOCAL_HTTP_BODY_DATA;
    }
    size_t remaining() const override { return length_ - offset_; }
    bool cancelled() const override { return false; }
private:
    const char *text_;
    size_t offset_;
    size_t length_;
};

class FallbackHandler : public LocalHttpHandler {
public:
    LocalHttpResponse handle(const char *, char *body, size_t) override {
        strcpy(body, "{\"telemetry\":true}");
        return {
            "200 OK", "application/json", strlen(body), NULL, NULL, false
        };
    }
};

LocalHttpRequest makeRequest(
    const char *line, const char *authorization, uint8_t count,
    uint32_t peer = 0x0a000001, uint32_t generation = 1) {
    static const char EMPTY[] = "";
    LocalHttpRequest request = {
        line, peer, generation, authorization, count,
        EMPTY, 0, "device", 1, EMPTY, 0, false,
        true, false, false, 0, 0
    };
    return request;
}

LocalHttpStreamingRequest makeStreamingRequest(
    const char *line,
    size_t contentLength,
    uint32_t generation,
    const LocalHttpRequest &metadata) {
    static const char EMPTY[] = "";
    LocalHttpStreamingRequest request = {
        line,
        contentLength,
        generation,
        {
            metadata.authorizationCount == 1
                ? LOCAL_HTTP_METADATA_VALID : LOCAL_HTTP_METADATA_ABSENT,
            metadata.authorization,
            metadata.authorization == NULL ? 0 : strlen(metadata.authorization)
        },
        {LOCAL_HTTP_METADATA_VALID, metadata.host, strlen(metadata.host)},
        {LOCAL_HTTP_METADATA_ABSENT, EMPTY, 0},
        {LOCAL_HTTP_PEER_IPV4_VALID, metadata.peerAddress},
        metadata
    };
    return request;
}

struct UploadContext {
    LocalOtaController *controller;
    LocalHttpStreamingRequest request;
    FakeBody *body;
    bool result;
};

void uploadThread(void *rawContext) {
    UploadContext *context = static_cast<UploadContext *>(rawContext);
    context->result =
        context->controller->upload(context->request, *context->body);
}

bool waitForState(LocalOtaController &controller, LocalOtaState expected) {
    for (int i = 0; i < 100; ++i) {
        if (controller.snapshot().state == expected) {
            return true;
        }
        delay(2);
    }
    return false;
}

void testFailClosedConfiguration() {
    FakeCore core;
    NetworkMaintenanceCoordinator network;
    LocalOtaController disabled(
        core, network, NULL, 0,
        "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock);
    expect(!disabled.begin(),
           "missing public key fails closed");

    uint8_t key[] = {1};
    LocalOtaController invalidVersion(
        core, network, key, sizeof(key),
        "HomeTemperature", "MXCHIP_AZ3166", "release", readClock);
    expect(!invalidVersion.begin(),
           "malformed running firmware version fails closed");
}

void testLeaseUploadRoutesAndApply() {
    fakeNow = 100;
    FakeCore core;
    NetworkMaintenanceCoordinator network;
    uint8_t key[] = {1, 2, 3};
    LocalOtaController controller(
        core, network, key, sizeof(key),
        "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock);
    expect(controller.begin(), "configured controller starts its joinable worker");
    LocalHttpRequest valid = makeRequest(
        "GET /api/ota/status HTTP/1.1", "", 0);

    FakeBody body(385);
    LocalHttpStreamingRequest uploadRequest =
        makeStreamingRequest("POST /api/ota HTTP/1.1", 385, 1, valid);
    uploadRequest.metadata.requestLine = uploadRequest.requestLine;
    UploadContext upload = {&controller, uploadRequest, &body, false};
    rtos::Thread thread;
    thread.start(mbed::callback(uploadThread, static_cast<void *>(&upload)));
    expect(waitForState(controller, LOCAL_OTA_WAITING_FOR_NETWORK_LEASE),
           "upload waits for the exclusive network lease");
    expect(!network.tryBeginCloud(),
           "OTA reservation blocks a new cloud upload");
    controller.update(1);
    thread.join();
    expect(upload.result && controller.snapshot().state == LOCAL_OTA_READY &&
               core.begins == 1 && core.finishes == 1,
           "worker alone streams and retains the verified Core session");

    FallbackHandler fallback;
    LocalOtaHttpHandler handler(fallback, controller);
    char pageBody[3072];
    LocalHttpRequest page = makeRequest("GET /ota HTTP/1.1", "", 0);
    LocalHttpResponse pageResponse =
        handler.handleRequest(page, pageBody, sizeof(pageBody));
    expect(strcmp(pageResponse.status, "200 OK") == 0 &&
               strcmp(pageResponse.contentType, "text/html; charset=utf-8") == 0 &&
               strstr(pageBody, "AZ3166 Local OTA") != NULL &&
               strstr(pageBody, "/api/ota/apply") != NULL &&
               strstr(pageBody, "['Error','Fatal']") != NULL &&
               strstr(pageBody, "Update check timed out") != NULL,
           "OTA page exposes the same-origin signed upload workflow");
    page.requestLine = "GET /ota?unsafe=1 HTTP/1.1";
    pageResponse = handler.handleRequest(page, pageBody, sizeof(pageBody));
    expect(strcmp(pageResponse.status, "404 Not Found") == 0,
           "OTA page rejects query-string route aliases");
    expect(handler.handles("POST /api/ota HTTP/1.1") &&
               handler.handles("POST /api/ota/apply HTTP/1.1") &&
               !handler.handles("POST /api/ota/session HTTP/1.1") &&
               !handler.handles("GET /api/telemetry HTTP/1.1"),
           "only OTA body routes transfer to the streaming worker");
    char responseBody[256];
    LocalHttpResponse response =
        handler.handleRequest(valid, responseBody, sizeof(responseBody));
    expect(strcmp(response.status, "200 OK") == 0 &&
               strstr(responseBody, "\"state\":\"Ready\"") != NULL &&
               strstr(responseBody, "\"generation\":7") != NULL &&
               strstr(responseBody,
                      "\"digest\":\"4200000000000000000000000000000000000000000000000000000000000000\"") != NULL,
           "authorized status reports the Core generation and digest");

    LocalHttpRequest malformedStatus = valid;
    malformedStatus.hasTransferEncoding = true;
    response = handler.handleRequest(
        malformedStatus, responseBody, sizeof(responseBody));
    expect(strcmp(response.status, "400 Bad Request") == 0 &&
               controller.snapshot().state == LOCAL_OTA_READY,
           "status rejects Transfer-Encoding before observing state");

    LocalHttpRequest malformedApply = valid;
    malformedApply.requestLine = "POST /api/ota/apply HTTP/1.1";
    malformedApply.contentType = "text/plain";
    malformedApply.contentTypeCount = 1;
    const char applyJson[] =
        "{\"generation\":7,\"digest\":\""
        "4200000000000000000000000000000000000000000000000000000000000000\"}";
    TextBody malformedApplyBody(applyJson);
    LocalHttpStreamingRequest malformedApplyRequest = makeStreamingRequest(
        malformedApply.requestLine, strlen(applyJson), 1, malformedApply);
    response = handler.handle(
        malformedApplyRequest, malformedApplyBody,
        responseBody, sizeof(responseBody));
    expect(strcmp(response.status, "400 Bad Request") == 0 &&
               core.activates == 0,
           "apply rejects an invalid content type before mutation");

    LocalHttpRequest malformedCancel = valid;
    malformedCancel.requestLine = "DELETE /api/ota HTTP/1.1";
    malformedCancel.hasContentLength = true;
    malformedCancel.contentLength = 1;
    response = handler.handleRequest(
        malformedCancel, responseBody, sizeof(responseBody));
    expect(strcmp(response.status, "400 Bad Request") == 0 &&
               controller.snapshot().state == LOCAL_OTA_READY,
           "cancel rejects Content-Length bodies before mutation");

    LocalHttpRequest apply = valid;
    apply.requestLine = "POST /api/ota/apply HTTP/1.1";
    apply.contentType = "application/json";
    apply.contentTypeCount = 1;
    const char wrongApplyJson[] =
        "{\"generation\":7,\"digest\":\""
        "4300000000000000000000000000000000000000000000000000000000000000\"}";
    TextBody wrongApplyBody(wrongApplyJson);
    LocalHttpStreamingRequest wrongApply = makeStreamingRequest(
        apply.requestLine, strlen(wrongApplyJson), 1, apply);
    response = handler.handle(
        wrongApply, wrongApplyBody, responseBody, sizeof(responseBody));
    expect(strcmp(response.status, "409 Conflict") == 0 &&
               core.activates == 0,
           "apply rejects a digest that does not match the staged image");

    TextBody applyBody(applyJson);
    LocalHttpStreamingRequest applyRequest = makeStreamingRequest(
        apply.requestLine, strlen(applyJson), 1, apply);
    response = handler.handle(
        applyRequest, applyBody, responseBody, sizeof(responseBody));
    expect(strcmp(response.status, "202 Accepted") == 0 &&
               core.activates == 0 && response.afterAttempt != NULL,
           "apply returns 202 before boot metadata persistence begins");
    response.afterAttempt(false, response.afterAttemptContext);
    expect(waitForState(controller, LOCAL_OTA_IDLE) && core.activates == 1 &&
               controller.takeRebootRequest(),
           "response completion releases activation and posts reboot");
    FakeBody replacementBody(385);
    expect(!controller.upload(uploadRequest, replacementBody) &&
               controller.takeRebootRequest(),
           "committed activation blocks uploads until reset");
    expect(network.tryBeginCloud(),
           "terminal activation releases the network lease");
    network.endCloud();
}

void testWaitingLeaseAndIdleReadCancellation() {
    fakeNow = 0;
    FakeCore core;
    NetworkMaintenanceCoordinator network;
    uint8_t key[] = {1};
    LocalOtaController controller(
        core, network, key, sizeof(key),
        "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock);
    controller.begin();
    LocalHttpRequest valid = makeRequest(
        "POST /api/ota HTTP/1.1", "", 0, 0x0a000001, 3);
    LocalHttpStreamingRequest request =
        makeStreamingRequest(valid.requestLine, 385, 3, valid);

    expect(network.tryBeginCloud(), "fake cloud upload owns the lease");
    FakeBody waitingBody(385);
    UploadContext waiting = {&controller, request, &waitingBody, false};
    rtos::Thread waitingThread;
    waitingThread.start(
        mbed::callback(uploadThread, static_cast<void *>(&waiting)));
    expect(waitForState(controller, LOCAL_OTA_WAITING_FOR_NETWORK_LEASE),
           "upload waits behind the in-flight cloud lease");
    waitingBody.cancel();
    waitingThread.join();
    expect(!waiting.result && controller.snapshot().state == LOCAL_OTA_IDLE,
           "server-shutdown body cancellation completes a waiting upload");
    network.endCloud();

    request.metadata = valid;
    IdleBody idleBody(385);
    UploadContext idle = {&controller, request, &idleBody, false};
    rtos::Thread idleThread;
    idleThread.start(mbed::callback(uploadThread, static_cast<void *>(&idle)));
    waitForState(controller, LOCAL_OTA_WAITING_FOR_NETWORK_LEASE);
    controller.update(3);
    expect(waitForState(controller, LOCAL_OTA_RECEIVING),
           "idle-read upload receives the OTA lease");
    controller.cancel();
    idleThread.join();
    expect(!idle.result && core.aborts >= 1 &&
               controller.snapshot().state == LOCAL_OTA_IDLE,
           "zero-byte body reads observe cancellation and abort promptly");
}

enum ApplyRace {
    APPLY_RACE_CANCEL,
    APPLY_RACE_NETWORK
};

struct ApplyRaceContext {
    LocalOtaController *controller;
    LocalHttpRequest request;
    ApplyRace race;
};

void invalidateBeforeApply(void *rawContext) {
    ApplyRaceContext *context =
        static_cast<ApplyRaceContext *>(rawContext);
    if (context->race == APPLY_RACE_CANCEL) {
        context->controller->cancel();
    } else {
        context->controller->update(context->request.networkGeneration + 1);
    }
}

void runApplyRace(int raceValue, const char *name) {
    ApplyRace race = static_cast<ApplyRace>(raceValue);
    fakeNow = 0;
    FakeCore core;
    NetworkMaintenanceCoordinator network;
    uint8_t key[] = {1};
    ApplyRaceContext hook = {};
    LocalOtaController controller(
        core, network, key, sizeof(key),
        "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock,
        invalidateBeforeApply, &hook);
    controller.begin();
    LocalHttpRequest valid = makeRequest(
        "POST /api/ota HTTP/1.1", "", 0, 0x0a000001, 6);
    FakeBody body(385);
    LocalHttpStreamingRequest request =
        makeStreamingRequest(valid.requestLine, 385, 6, valid);
    UploadContext upload = {&controller, request, &body, false};
    rtos::Thread thread;
    thread.start(mbed::callback(uploadThread, static_cast<void *>(&upload)));
    waitForState(controller, LOCAL_OTA_WAITING_FOR_NETWORK_LEASE);
    controller.update(6);
    thread.join();
    valid.requestLine = "POST /api/ota/apply HTTP/1.1";
    hook.controller = &controller;
    hook.request = valid;
    hook.race = race;
    OTAStagingError result = OTA_OK;
    uint8_t digest[OTA_SHA256_SIZE] = {0x42};
    void *responseContext = NULL;
    bool accepted = controller.apply(7, digest, result, responseContext);
    LocalOtaController::responseAttempted(false, responseContext);
    expect(accepted && result == OTA_OK &&
               waitForState(controller, LOCAL_OTA_IDLE) &&
               core.activates == 0 &&
               !controller.takeRebootRequest(),
           name);
}

void testFinalApplyRevalidationRaces() {
    runApplyRace(
        APPLY_RACE_CANCEL,
        "explicit cancel wins immediately before Ready to Applying");
    runApplyRace(
        APPLY_RACE_NETWORK,
        "network generation change wins immediately before Ready to Applying");
}

void testStaleSignalCannotReleaseLaterApply() {
    fakeNow = 0;
    FakeCore core;
    NetworkMaintenanceCoordinator network;
    uint8_t key[] = {1};
    LocalOtaController controller(
        core, network, key, sizeof(key),
        "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock);
    controller.begin();
    LocalHttpRequest valid = makeRequest(
        "POST /api/ota HTTP/1.1", "", 0, 0x0a000001, 8);
    LocalHttpStreamingRequest request =
        makeStreamingRequest(valid.requestLine, 385, 8, valid);
    FakeBody firstBody(385);
    UploadContext firstUpload = {&controller, request, &firstBody, false};
    rtos::Thread firstThread;
    firstThread.start(
        mbed::callback(uploadThread, static_cast<void *>(&firstUpload)));
    waitForState(controller, LOCAL_OTA_WAITING_FOR_NETWORK_LEASE);
    controller.update(8);
    firstThread.join();

    OTAStagingError result = OTA_OK;
    uint8_t digest[OTA_SHA256_SIZE] = {0x42};
    void *firstResponseContext = NULL;
    expect(controller.apply(
               7, digest, result, firstResponseContext) &&
               controller.cancel(),
           "cancel can invalidate an apply before its response callback");
    expect(waitForState(controller, LOCAL_OTA_IDLE) && core.activates == 0,
           "cancelled apply does not activate before its delayed callback");

    request.metadata.networkGeneration = 8;
    FakeBody secondBody(385);
    UploadContext secondUpload = {&controller, request, &secondBody, false};
    rtos::Thread secondThread;
    secondThread.start(
        mbed::callback(uploadThread, static_cast<void *>(&secondUpload)));
    waitForState(controller, LOCAL_OTA_WAITING_FOR_NETWORK_LEASE);
    controller.update(8);
    secondThread.join();
    void *secondResponseContext = NULL;
    expect(controller.apply(
               7, digest, result, secondResponseContext),
           "a replacement apply can be queued after cancellation");
    LocalOtaController::responseAttempted(false, firstResponseContext);
    delay(20);
    expect(core.activates == 0 &&
               controller.snapshot().state == LOCAL_OTA_READY,
           "a stale response callback cannot release a later apply");
    LocalOtaController::responseAttempted(false, secondResponseContext);
    expect(waitForState(controller, LOCAL_OTA_IDLE) && core.activates == 1 &&
               controller.takeRebootRequest(),
           "the matching response callback releases the later apply");
}

void runPostResponseActivationFailure(
    OTAStagingError activationResult,
    LocalOtaState expectedState,
    bool expectedLeaseBusy,
    const char *name) {
    fakeNow = 0;
    FakeCore core;
    core.activationResult = activationResult;
    NetworkMaintenanceCoordinator network;
    uint8_t key[] = {1};
    LocalOtaController controller(
        core, network, key, sizeof(key),
        "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock);
    controller.begin();
    LocalHttpRequest valid = makeRequest(
        "POST /api/ota HTTP/1.1", "", 0, 0x0a000001, 10);
    LocalHttpStreamingRequest request =
        makeStreamingRequest(valid.requestLine, 385, 10, valid);
    FakeBody body(385);
    UploadContext upload = {&controller, request, &body, false};
    rtos::Thread thread;
    thread.start(mbed::callback(uploadThread, static_cast<void *>(&upload)));
    waitForState(controller, LOCAL_OTA_WAITING_FOR_NETWORK_LEASE);
    controller.update(10);
    thread.join();

    OTAStagingError result = OTA_OK;
    uint8_t digest[OTA_SHA256_SIZE] = {0x42};
    void *responseContext = NULL;
    bool accepted = controller.apply(7, digest, result, responseContext);
    LocalOtaController::responseAttempted(false, responseContext);
    LocalOtaSnapshot snapshot;
    bool reachedExpected = false;
    for (int i = 0; i < 100; ++i) {
        snapshot = controller.snapshot();
        if (snapshot.state == expectedState) {
            reachedExpected = true;
            break;
        }
        delay(2);
    }
    expect(accepted && reachedExpected && core.activates == 1 &&
               snapshot.lastError == activationResult &&
               !controller.takeRebootRequest() &&
               network.otaBusy() == expectedLeaseBusy,
           name);
}

void testPostResponseActivationFailures() {
    runPostResponseActivationFailure(
        OTA_ERROR_ACTIVATION, LOCAL_OTA_ERROR, false,
        "verified activation failure releases the lease without reboot");
    runPostResponseActivationFailure(
        OTA_ERROR_ACTIVATION_UNCERTAIN, LOCAL_OTA_FATAL, true,
        "uncertain activation remains fatal with lease held and no reboot");
}

void testTypedErrorsAndRebootCloudPolicy() {
    FakeCore core;
    NetworkMaintenanceCoordinator network;
    uint8_t key[] = {1};
    LocalOtaController controller(
        core, network, key, sizeof(key),
        "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock);
    expect(strcmp(controller.errorName(OTA_ERROR_WRITE), "OTA_ERROR_WRITE") == 0 &&
               strcmp(controller.errorName(OTA_ERROR_SIGNATURE),
                      "OTA_ERROR_SIGNATURE") == 0 &&
               core.errorNames == 2,
           "typed Core error names are delegated without collapsing details");

    OtaRebootCoordinator reboot(250);
    expect(reboot.cloudAllowed() && !reboot.pending(),
           "cloud is allowed before activation schedules reboot");
    reboot.schedule(1000);
    expect(reboot.pending() && !reboot.cloudAllowed() &&
               !reboot.due(1249) && reboot.due(1250),
           "pending reboot excludes cloud before its bounded reset deadline");
}

void testPublicCancelHttpRoute() {
    fakeNow = 0;
    FakeCore core;
    NetworkMaintenanceCoordinator network;
    uint8_t key[] = {1};
    LocalOtaController controller(
        core, network, key, sizeof(key),
        "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock);
    controller.begin();
    FallbackHandler fallback;
    LocalOtaHttpHandler handler(fallback, controller);

    LocalHttpRequest metadata = makeRequest(
        "POST /api/ota HTTP/1.1", "", 0, 0x0a000002, 9);
    FakeBody body(385);
    LocalHttpStreamingRequest upload =
        makeStreamingRequest(metadata.requestLine, 385, 9, metadata);
    UploadContext context = {&controller, upload, &body, false};
    rtos::Thread thread;
    thread.start(mbed::callback(uploadThread, static_cast<void *>(&context)));
    expect(waitForState(controller, LOCAL_OTA_WAITING_FOR_NETWORK_LEASE),
           "public upload starts without a session claim");

    LocalHttpRequest cancel = makeRequest(
        "DELETE /api/ota HTTP/1.1", "", 0, 0x0a000002, 9);
    char responseBody[256];
    LocalHttpResponse response =
        handler.handleRequest(cancel, responseBody, sizeof(responseBody));
    thread.join();
    expect(strcmp(response.status, "202 Accepted") == 0 &&
               waitForState(controller, LOCAL_OTA_IDLE),
           "public DELETE route cancels the active upload");
}

void testNetworkGenerationCancelsReadyWorker() {
    fakeNow = 0;
    FakeCore core;
    NetworkMaintenanceCoordinator network;
    uint8_t key[] = {1};
    LocalOtaController controller(
        core, network, key, sizeof(key),
        "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock);
    controller.begin();
    LocalHttpRequest valid = makeRequest(
        "POST /api/ota HTTP/1.1", "", 0, 0x0a000001, 4);
    FakeBody body(385);
    LocalHttpStreamingRequest request =
        makeStreamingRequest("POST /api/ota HTTP/1.1", 385, 4, valid);
    UploadContext upload = {&controller, request, &body, false};
    rtos::Thread thread;
    thread.start(mbed::callback(uploadThread, static_cast<void *>(&upload)));
    waitForState(controller, LOCAL_OTA_WAITING_FOR_NETWORK_LEASE);
    controller.update(4);
    thread.join();
    controller.update(5);
    expect(waitForState(controller, LOCAL_OTA_IDLE) && core.aborts == 1 &&
               !network.otaBusy(),
           "network generation change cancels Ready and releases the lease");
}

void setup() {
    Serial.begin(115200);
    while (!Serial);
    delay(3000);
    Serial.println("TEST_SUITE: LocalOtaControllerTests");
    testFailClosedConfiguration();
    testLeaseUploadRoutesAndApply();
    testWaitingLeaseAndIdleReadCancellation();
    testFinalApplyRevalidationRaces();
    testStaleSignalCannotReleaseLaterApply();
    testPostResponseActivationFailures();
    testTypedErrorsAndRebootCloudPolicy();
    testPublicCancelHttpRoute();
    testNetworkGenerationCancelsReadyWorker();
}

void loop() {
    Serial.print("TEST_RESULT: ");
    Serial.println(failureCount == 0 ? "PASS" : "FAIL");
    delay(1000);
}
