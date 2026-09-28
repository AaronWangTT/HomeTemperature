#include <Arduino.h>
#include <string.h>

#include "src/ota/LocalOtaController.h"
#include "src/ota/LocalOtaHttpHandler.h"
#include "src/ota/NetworkMaintenanceCoordinator.h"
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

class FakeEntropy : public LocalOtaEntropy {
public:
    uint8_t next = 1;
    bool fail = false;
    bool fill(uint8_t *output, size_t size) override {
        if (fail) {
            return false;
        }
        for (size_t i = 0; i < size; ++i) {
            output[i] = next++;
        }
        return true;
    }
};

class FakeDisplay : public LocalOtaDisplay {
public:
    char shown[9] = {};
    int clears = 0;
    void showChallenge(const char *challenge) override {
        strncpy(shown, challenge, sizeof(shown) - 1);
    }
    void clearChallenge() override {
        shown[0] = '\0';
        ++clears;
    }
};

class FakeCore : public LocalOtaCore {
public:
    int begins = 0;
    int writes = 0;
    int finishes = 0;
    int activates = 0;
    int aborts = 0;
    int errorNames = 0;
    OTAStagingError activationResult = OTA_OK;
    rtos::Semaphore *activationEntered = NULL;
    rtos::Semaphore *activationGate = NULL;

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
        return admission(&metadata, context) == 0
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
        if (activationEntered != NULL) {
            activationEntered->release();
        }
        if (activationGate != NULL) {
            activationGate->wait(1000);
        }
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

struct ApplyContext {
    LocalOtaController *controller;
    LocalHttpRequest request;
    OTAStagingError result;
    bool accepted;
};

void uploadThread(void *rawContext) {
    UploadContext *context = static_cast<UploadContext *>(rawContext);
    context->result =
        context->controller->upload(context->request, *context->body);
}

void applyThread(void *rawContext) {
    ApplyContext *context = static_cast<ApplyContext *>(rawContext);
    context->accepted =
        context->controller->apply(context->request, context->result);
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

bool stageReady(
    LocalOtaController &controller,
    uint32_t networkGeneration,
    char authorization[37],
    LocalHttpRequest &request) {
    if (!controller.begin() || !controller.openChallenge()) {
        return false;
    }
    char capability[33];
    if (!controller.claim(
            "01020304", 0x0a000001, networkGeneration, capability)) {
        return false;
    }
    snprintf(authorization, 37, "OTA %s", capability);
    request = makeRequest(
        "POST /api/ota HTTP/1.1", authorization, 1,
        0x0a000001, networkGeneration);
    FakeBody body(385);
    LocalHttpStreamingRequest streaming =
        makeStreamingRequest(
            request.requestLine, 385, networkGeneration, request);
    UploadContext upload = {&controller, streaming, &body, false};
    rtos::Thread thread;
    thread.start(mbed::callback(uploadThread, static_cast<void *>(&upload)));
    if (!waitForState(
            controller, LOCAL_OTA_WAITING_FOR_NETWORK_LEASE)) {
        thread.join();
        return false;
    }
    controller.update(networkGeneration);
    thread.join();
    return upload.result &&
        controller.snapshot().state == LOCAL_OTA_READY;
}

class BlockingApplyValidation {
public:
    BlockingApplyValidation() : entered(0), gate(0) {}
    rtos::Semaphore entered;
    rtos::Semaphore gate;
};

void blockApplyValidation(void *context) {
    BlockingApplyValidation *blocking =
        static_cast<BlockingApplyValidation *>(context);
    blocking->entered.release();
    blocking->gate.wait(1000);
}

void testFailClosedConfigurationAndEntropy() {
    FakeCore core;
    FakeEntropy entropy;
    FakeDisplay display;
    NetworkMaintenanceCoordinator network;
    LocalOtaController disabled(
        core, entropy, display, network, NULL, 0,
        "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock);
    expect(!disabled.begin() && !disabled.openChallenge(),
           "missing public key fails closed");

    uint8_t key[] = {1};
    LocalOtaController controller(
        core, entropy, display, network, key, sizeof(key),
        "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock);
    expect(controller.begin(), "configured controller starts its joinable worker");
    entropy.fail = true;
    expect(!controller.openChallenge() &&
               controller.snapshot().state == LOCAL_OTA_IDLE,
           "TRNG failure leaves authorization disabled");
}

void testAuthorizationLeaseUploadRoutesAndApply() {
    fakeNow = 100;
    FakeCore core;
    FakeEntropy entropy;
    FakeDisplay display;
    NetworkMaintenanceCoordinator network;
    uint8_t key[] = {1, 2, 3};
    LocalOtaController controller(
        core, entropy, display, network, key, sizeof(key),
        "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock);
    expect(controller.begin() && controller.openChallenge() &&
               strcmp(display.shown, "01020304") == 0,
           "physical initiation displays deterministic TRNG challenge");

    char capability[33];
    expect(!controller.claim("01020305", 0x0a000001, 1, capability),
           "wrong challenge is rejected");
    expect(controller.claim("01020304", 0x0a000001, 1, capability) &&
               strcmp(capability, "05060708090a0b0c0d0e0f1011121314") == 0 &&
               display.clears == 1,
           "correct challenge creates a source-bound 128-bit capability");

    char authorization[37];
    snprintf(authorization, sizeof(authorization), "OTA %s", capability);
    LocalHttpRequest valid = makeRequest(
        "GET /api/ota/status HTTP/1.1", authorization, 1);
    uint32_t generation;
    expect(controller.authorize(valid, generation),
           "exact lowercase Authorization header is accepted");
    LocalHttpRequest duplicate = valid;
    duplicate.authorizationCount = 2;
    expect(!controller.authorize(duplicate, generation),
           "duplicate Authorization headers fail closed");
    LocalHttpRequest wrongSource = valid;
    wrongSource.peerAddress++;
    expect(!controller.authorize(wrongSource, generation),
           "capability is bound to the claiming source");

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
    expect(handler.handles("POST /api/ota HTTP/1.1") &&
               handler.handles("POST /api/ota/session HTTP/1.1") &&
               !handler.handles("GET /api/telemetry HTTP/1.1"),
           "only OTA body routes transfer to the streaming worker");
    char responseBody[256];
    LocalHttpResponse response =
        handler.handleRequest(valid, responseBody, sizeof(responseBody));
    expect(strcmp(response.status, "200 OK") == 0 &&
               strstr(responseBody, "\"state\":\"Ready\"") != NULL,
           "authorized status route reports Ready");

    LocalHttpRequest malformedStatus = valid;
    malformedStatus.hasTransferEncoding = true;
    response = handler.handleRequest(
        malformedStatus, responseBody, sizeof(responseBody));
    expect(strcmp(response.status, "400 Bad Request") == 0 &&
               controller.snapshot().state == LOCAL_OTA_READY,
           "status rejects Transfer-Encoding before observing state");

    LocalHttpRequest malformedApply = valid;
    malformedApply.requestLine = "POST /api/ota/apply HTTP/1.1";
    malformedApply.prefetchedLength = 1;
    response = handler.handleRequest(
        malformedApply, responseBody, sizeof(responseBody));
    expect(strcmp(response.status, "400 Bad Request") == 0 &&
               core.activates == 0,
           "apply rejects prefetched body bytes before mutation");

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
    response = handler.handleRequest(apply, responseBody, sizeof(responseBody));
    expect(strcmp(response.status, "202 Accepted") == 0 &&
               core.activates == 1 && response.afterAttempt != NULL,
           "apply is executed by the OTA worker and returns 202");
    expect(controller.snapshot().state ==
               LOCAL_OTA_ACTIVATED_AWAITING_RESPONSE &&
               !controller.openChallenge() &&
               !network.tryBeginCloud() &&
               !controller.takeRebootRequest(),
           "activation success retains OTA exclusion until response attempt");
    response.afterAttempt(false, response.afterAttemptContext);
    expect(controller.snapshot().rebootPending &&
               !controller.openChallenge() &&
               !network.tryBeginCloud(),
           "failed response send still posts reboot without opening a race");
    expect(controller.takeRebootRequest() &&
               controller.snapshot().state ==
                   LOCAL_OTA_ACTIVATED_AWAITING_RESPONSE,
           "main-loop reboot handoff consumes the request without returning Idle");
    OtaRebootCoordinator reboot(250);
    reboot.schedule(fakeNow);
    expect(!reboot.cloudAllowed(),
           "scheduled reboot suppresses cloud after the OTA lease handoff");
    expect(network.tryBeginCloud(),
           "reboot handoff releases the OTA lease at the controlled point");
    network.endCloud();
}

void testWaitingLeaseAndIdleReadCancellation() {
    fakeNow = 0;
    FakeCore core;
    FakeEntropy entropy;
    FakeDisplay display;
    NetworkMaintenanceCoordinator network;
    uint8_t key[] = {1};
    LocalOtaController controller(
        core, entropy, display, network, key, sizeof(key),
        "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock);
    controller.begin();
    controller.openChallenge();
    char capability[33];
    controller.claim("01020304", 0x0a000001, 3, capability);
    char authorization[37];
    snprintf(authorization, sizeof(authorization), "OTA %s", capability);
    LocalHttpRequest valid = makeRequest(
        "POST /api/ota HTTP/1.1", authorization, 1, 0x0a000001, 3);
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

    entropy.next = 1;
    controller.openChallenge();
    controller.claim("01020304", 0x0a000001, 3, capability);
    snprintf(authorization, sizeof(authorization), "OTA %s", capability);
    valid.authorization = authorization;
    request.metadata = valid;
    IdleBody idleBody(385);
    UploadContext idle = {&controller, request, &idleBody, false};
    rtos::Thread idleThread;
    idleThread.start(mbed::callback(uploadThread, static_cast<void *>(&idle)));
    waitForState(controller, LOCAL_OTA_WAITING_FOR_NETWORK_LEASE);
    controller.update(3);
    expect(waitForState(controller, LOCAL_OTA_RECEIVING),
           "idle-read upload receives the OTA lease");
    controller.cancel(valid);
    idleThread.join();
    expect(!idle.result && core.aborts >= 1 &&
               controller.snapshot().state == LOCAL_OTA_IDLE,
           "zero-byte body reads observe cancellation and abort promptly");
}

enum ApplyRace {
    APPLY_RACE_EXPIRY,
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
    if (context->race == APPLY_RACE_EXPIRY) {
        fakeNow = LocalOtaController::CAPABILITY_WINDOW_MS + 1;
        context->controller->update(context->request.networkGeneration);
    } else if (context->race == APPLY_RACE_CANCEL) {
        context->controller->cancel(context->request);
    } else {
        context->controller->update(context->request.networkGeneration + 1);
    }
}

void runApplyRace(int raceValue, const char *name) {
    ApplyRace race = static_cast<ApplyRace>(raceValue);
    fakeNow = 0;
    FakeCore core;
    FakeEntropy entropy;
    FakeDisplay display;
    NetworkMaintenanceCoordinator network;
    uint8_t key[] = {1};
    ApplyRaceContext hook = {};
    LocalOtaController controller(
        core, entropy, display, network, key, sizeof(key),
        "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock,
        invalidateBeforeApply, &hook);
    controller.begin();
    controller.openChallenge();
    char capability[33];
    controller.claim("01020304", 0x0a000001, 6, capability);
    char authorization[37];
    snprintf(authorization, sizeof(authorization), "OTA %s", capability);
    LocalHttpRequest valid = makeRequest(
        "POST /api/ota HTTP/1.1", authorization, 1, 0x0a000001, 6);
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
    bool accepted = controller.apply(valid, result);
    expect(accepted && result == OTA_ERROR_CANCELLED &&
               core.activates == 0 &&
               controller.snapshot().state == LOCAL_OTA_IDLE,
           name);
}

void testFinalApplyRevalidationRaces() {
    runApplyRace(
        APPLY_RACE_EXPIRY,
        "capability expiry wins immediately before Ready to Applying");
    runApplyRace(
        APPLY_RACE_CANCEL,
        "explicit cancel wins immediately before Ready to Applying");
    runApplyRace(
        APPLY_RACE_NETWORK,
        "network generation change wins immediately before Ready to Applying");
}

void testTypedErrorsAndRebootCloudPolicy() {
    FakeCore core;
    FakeEntropy entropy;
    FakeDisplay display;
    NetworkMaintenanceCoordinator network;
    uint8_t key[] = {1};
    LocalOtaController controller(
        core, entropy, display, network, key, sizeof(key),
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

void testActivatedAwaitingResponseShutdown() {
    fakeNow = 0;
    FakeCore core;
    FakeEntropy entropy;
    FakeDisplay display;
    NetworkMaintenanceCoordinator network;
    uint8_t key[] = {1};
    {
        LocalOtaController controller(
            core, entropy, display, network, key, sizeof(key),
            "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock);
        controller.begin();
        controller.openChallenge();
        char capability[33];
        controller.claim("01020304", 0x0a000001, 8, capability);
        char authorization[37];
        snprintf(authorization, sizeof(authorization), "OTA %s", capability);
        LocalHttpRequest request = makeRequest(
            "POST /api/ota HTTP/1.1", authorization, 1, 0x0a000001, 8);
        FakeBody body(385);
        LocalHttpStreamingRequest uploadRequest =
            makeStreamingRequest(request.requestLine, 385, 8, request);
        UploadContext upload = {&controller, uploadRequest, &body, false};
        rtos::Thread uploadWorker;
        uploadWorker.start(
            mbed::callback(uploadThread, static_cast<void *>(&upload)));
        waitForState(controller, LOCAL_OTA_WAITING_FOR_NETWORK_LEASE);
        controller.update(8);
        uploadWorker.join();
        request.requestLine = "POST /api/ota/apply HTTP/1.1";
        OTAStagingError result = OTA_ERROR_INVALID_STATE;
        expect(controller.apply(request, result) && result == OTA_OK &&
                   controller.snapshot().state ==
                       LOCAL_OTA_ACTIVATED_AWAITING_RESPONSE &&
                   !network.tryBeginCloud(),
               "activation-awaiting-response retains lease before shutdown");
    }
    expect(network.tryBeginCloud(),
           "bounded controller shutdown releases awaiting-response lease");
    network.endCloud();
}

void testShutdownReleasesPreActivationReservations() {
    uint8_t key[] = {1};

    {
        fakeNow = 0;
        FakeCore core;
        FakeEntropy entropy;
        FakeDisplay display;
        NetworkMaintenanceCoordinator network;
        LocalOtaController controller(
            core, entropy, display, network, key, sizeof(key),
            "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock);
        controller.begin();
        controller.openChallenge();
        char capability[33];
        controller.claim("01020304", 0x0a000001, 10, capability);
        char authorization[37];
        snprintf(authorization, sizeof(authorization), "OTA %s", capability);
        LocalHttpRequest request = makeRequest(
            "POST /api/ota HTTP/1.1", authorization, 1, 0x0a000001, 10);
        expect(network.tryBeginCloud(), "waiting shutdown holds a cloud lease");
        FakeBody body(385);
        LocalHttpStreamingRequest streaming =
            makeStreamingRequest(request.requestLine, 385, 10, request);
        UploadContext upload = {&controller, streaming, &body, false};
        rtos::Thread thread;
        thread.start(mbed::callback(uploadThread, static_cast<void *>(&upload)));
        waitForState(controller, LOCAL_OTA_WAITING_FOR_NETWORK_LEASE);
        controller.shutdown();
        thread.join();
        expect(!network.otaBusy(),
               "waiting shutdown releases its generation-bound OTA reservation");
        network.endCloud();
        expect(network.tryBeginCloud(),
               "cloud becomes available after waiting shutdown");
        network.endCloud();
    }

    {
        fakeNow = 0;
        FakeCore core;
        FakeEntropy entropy;
        FakeDisplay display;
        NetworkMaintenanceCoordinator network;
        LocalOtaController controller(
            core, entropy, display, network, key, sizeof(key),
            "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock);
        controller.begin();
        controller.openChallenge();
        char capability[33];
        controller.claim("01020304", 0x0a000001, 11, capability);
        char authorization[37];
        snprintf(authorization, sizeof(authorization), "OTA %s", capability);
        LocalHttpRequest request = makeRequest(
            "POST /api/ota HTTP/1.1", authorization, 1, 0x0a000001, 11);
        IdleBody body(385);
        LocalHttpStreamingRequest streaming =
            makeStreamingRequest(request.requestLine, 385, 11, request);
        UploadContext upload = {&controller, streaming, &body, false};
        rtos::Thread thread;
        thread.start(mbed::callback(uploadThread, static_cast<void *>(&upload)));
        waitForState(controller, LOCAL_OTA_WAITING_FOR_NETWORK_LEASE);
        controller.update(11);
        waitForState(controller, LOCAL_OTA_RECEIVING);
        controller.shutdown();
        thread.join();
        expect(core.aborts == 1 && !network.otaBusy() &&
                   network.tryBeginCloud(),
               "receiving shutdown aborts Core and releases cloud exclusion");
        network.endCloud();
    }

    {
        fakeNow = 0;
        FakeCore core;
        FakeEntropy entropy;
        FakeDisplay display;
        NetworkMaintenanceCoordinator network;
        LocalOtaController controller(
            core, entropy, display, network, key, sizeof(key),
            "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock);
        controller.begin();
        controller.openChallenge();
        char capability[33];
        controller.claim("01020304", 0x0a000001, 12, capability);
        char authorization[37];
        snprintf(authorization, sizeof(authorization), "OTA %s", capability);
        LocalHttpRequest request = makeRequest(
            "POST /api/ota HTTP/1.1", authorization, 1, 0x0a000001, 12);
        FakeBody body(385);
        LocalHttpStreamingRequest streaming =
            makeStreamingRequest(request.requestLine, 385, 12, request);
        UploadContext upload = {&controller, streaming, &body, false};
        rtos::Thread thread;
        thread.start(mbed::callback(uploadThread, static_cast<void *>(&upload)));
        waitForState(controller, LOCAL_OTA_WAITING_FOR_NETWORK_LEASE);
        controller.update(12);
        thread.join();
        expect(controller.snapshot().state == LOCAL_OTA_READY,
               "ready shutdown test stages an image");
        controller.shutdown();
        expect(core.aborts == 1 && !network.otaBusy() &&
                   network.tryBeginCloud(),
               "ready shutdown aborts Core and releases cloud exclusion");
        network.endCloud();
    }
}

struct UploadHandoffContext {
    LocalOtaController *controller;
    LocalHttpRequest request;
    bool triggerAfterFinish;
    bool invalidateNetwork;
    bool triggered;
};

void invalidateUploadHandoff(bool afterFinish, void *rawContext) {
    UploadHandoffContext *context =
        static_cast<UploadHandoffContext *>(rawContext);
    if (context->triggered || afterFinish != context->triggerAfterFinish) {
        return;
    }
    context->triggered = true;
    if (context->invalidateNetwork) {
        context->controller->update(context->request.networkGeneration + 1);
    } else {
        context->controller->cancel(context->request);
    }
}

void runUploadHandoffRace(
    bool afterFinish,
    bool invalidateNetwork,
    const char *name) {
    fakeNow = 0;
    FakeCore core;
    FakeEntropy entropy;
    FakeDisplay display;
    NetworkMaintenanceCoordinator network;
    uint8_t key[] = {1};
    UploadHandoffContext hook = {};
    LocalOtaController controller(
        core, entropy, display, network, key, sizeof(key),
        "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock,
        NULL, NULL, invalidateUploadHandoff, &hook);
    controller.begin();
    controller.openChallenge();
    char capability[33];
    controller.claim("01020304", 0x0a000001, 13, capability);
    char authorization[37];
    snprintf(authorization, sizeof(authorization), "OTA %s", capability);
    LocalHttpRequest request = makeRequest(
        "POST /api/ota HTTP/1.1", authorization, 1, 0x0a000001, 13);
    hook.controller = &controller;
    hook.request = request;
    hook.triggerAfterFinish = afterFinish;
    hook.invalidateNetwork = invalidateNetwork;
    FakeBody body(385);
    LocalHttpStreamingRequest streaming =
        makeStreamingRequest(request.requestLine, 385, 13, request);
    UploadContext upload = {&controller, streaming, &body, false};
    rtos::Thread thread;
    thread.start(mbed::callback(uploadThread, static_cast<void *>(&upload)));
    waitForState(controller, LOCAL_OTA_WAITING_FOR_NETWORK_LEASE);
    controller.update(13);
    thread.join();
    expect(hook.triggered && !upload.result &&
               controller.snapshot().state == LOCAL_OTA_IDLE &&
               core.aborts == 1 &&
               core.finishes == (afterFinish ? 1 : 0) &&
               network.tryBeginCloud(),
           name);
    network.endCloud();
}

void testUploadVerificationHandoffInvalidation() {
    runUploadHandoffRace(
        false, false,
        "cancel before Verifying aborts without calling finish");
    runUploadHandoffRace(
        false, true,
        "network invalidation before Verifying aborts without calling finish");
    runUploadHandoffRace(
        true, false,
        "cancel during finish wins before Ready publication");
    runUploadHandoffRace(
        true, true,
        "network invalidation during finish wins before Ready publication");
}

void testFatalActivationShutdownRetainsReservation() {
    fakeNow = 0;
    FakeCore core;
    core.activationResult = OTA_ERROR_ACTIVATION_UNCERTAIN;
    FakeEntropy entropy;
    FakeDisplay display;
    NetworkMaintenanceCoordinator network;
    uint8_t key[] = {1};
    LocalOtaController controller(
        core, entropy, display, network, key, sizeof(key),
        "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock);
    controller.begin();
    controller.openChallenge();
    char capability[33];
    controller.claim("01020304", 0x0a000001, 14, capability);
    char authorization[37];
    snprintf(authorization, sizeof(authorization), "OTA %s", capability);
    LocalHttpRequest request = makeRequest(
        "POST /api/ota HTTP/1.1", authorization, 1, 0x0a000001, 14);
    FakeBody body(385);
    LocalHttpStreamingRequest streaming =
        makeStreamingRequest(request.requestLine, 385, 14, request);
    UploadContext upload = {&controller, streaming, &body, false};
    rtos::Thread thread;
    thread.start(mbed::callback(uploadThread, static_cast<void *>(&upload)));
    waitForState(controller, LOCAL_OTA_WAITING_FOR_NETWORK_LEASE);
    controller.update(14);
    thread.join();
    request.requestLine = "POST /api/ota/apply HTTP/1.1";
    OTAStagingError result = OTA_OK;
    expect(controller.apply(request, result) &&
               result == OTA_ERROR_ACTIVATION_UNCERTAIN &&
               controller.snapshot().state == LOCAL_OTA_FATAL,
           "uncertain activation enters fatal maintenance");
    controller.shutdown();
    expect(network.otaBusy() && !network.tryBeginCloud(),
           "fatal shutdown deliberately retains OTA network exclusion");
}

void testApplyTimeoutBeforeWorkerValidationPreservesFatal() {
    fakeNow = 0;
    FakeCore core;
    FakeEntropy entropy;
    FakeDisplay display;
    NetworkMaintenanceCoordinator network;
    BlockingApplyValidation blocking;
    uint8_t key[] = {1};
    LocalOtaController controller(
        core, entropy, display, network, key, sizeof(key),
        "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock,
        blockApplyValidation, &blocking, NULL, NULL, 5);
    char authorization[37];
    LocalHttpRequest request = {};
    expect(stageReady(controller, 15, authorization, request),
           "pre-validation timeout test reaches Ready");
    request.requestLine = "POST /api/ota/apply HTTP/1.1";
    ApplyContext apply = {
        &controller, request, OTA_ERROR_INVALID_STATE, false
    };
    rtos::Thread thread;
    thread.start(mbed::callback(applyThread, static_cast<void *>(&apply)));
    expect(blocking.entered.wait(1000) > 0,
           "apply worker pauses before queued validation");
    thread.join();
    expect(apply.accepted &&
               apply.result == OTA_ERROR_ACTIVATION_UNCERTAIN &&
               controller.snapshot().state == LOCAL_OTA_FATAL &&
               network.otaBusy() && !network.tryBeginCloud() &&
               core.activates == 0 && !controller.takeRebootRequest(),
           "timeout before validation publishes only retained fatal uncertainty");
    blocking.gate.release();
    controller.shutdown();
    expect(controller.snapshot().state == LOCAL_OTA_FATAL &&
               core.aborts == 1 && network.otaBusy() &&
               !network.tryBeginCloud() &&
               !controller.takeRebootRequest(),
           "late validation abort cannot downgrade fatal or release exclusion");
}

void testApplyTimeoutDuringActivatePreservesFatal() {
    fakeNow = 0;
    FakeCore core;
    rtos::Semaphore activationEntered(0);
    rtos::Semaphore activationGate(0);
    core.activationEntered = &activationEntered;
    core.activationGate = &activationGate;
    FakeEntropy entropy;
    FakeDisplay display;
    NetworkMaintenanceCoordinator network;
    uint8_t key[] = {1};
    LocalOtaController controller(
        core, entropy, display, network, key, sizeof(key),
        "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock,
        NULL, NULL, NULL, NULL, 5);
    char authorization[37];
    LocalHttpRequest request = {};
    expect(stageReady(controller, 16, authorization, request),
           "late activation timeout test reaches Ready");
    request.requestLine = "POST /api/ota/apply HTTP/1.1";
    ApplyContext apply = {
        &controller, request, OTA_ERROR_INVALID_STATE, false
    };
    rtos::Thread thread;
    thread.start(mbed::callback(applyThread, static_cast<void *>(&apply)));
    expect(activationEntered.wait(1000) > 0,
           "Core activation pauses after validation");
    thread.join();
    expect(apply.accepted &&
               apply.result == OTA_ERROR_ACTIVATION_UNCERTAIN &&
               controller.snapshot().state == LOCAL_OTA_FATAL &&
               network.otaBusy() && !network.tryBeginCloud() &&
               !controller.takeRebootRequest(),
           "timeout during activate enters retained fatal uncertainty");
    activationGate.release();
    controller.shutdown();
    expect(controller.snapshot().state == LOCAL_OTA_FATAL &&
               network.otaBusy() && !network.tryBeginCloud() &&
               !controller.takeRebootRequest(),
           "late activation success cannot publish reboot or release exclusion");
}

void testClaimAndCancelHttpRoutes() {
    fakeNow = 0;
    FakeCore core;
    FakeEntropy entropy;
    FakeDisplay display;
    NetworkMaintenanceCoordinator network;
    uint8_t key[] = {1};
    LocalOtaController controller(
        core, entropy, display, network, key, sizeof(key),
        "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock);
    controller.begin();
    controller.openChallenge();
    FallbackHandler fallback;
    LocalOtaHttpHandler handler(fallback, controller);

    LocalHttpRequest claimMetadata = makeRequest(
        "POST /api/ota/session HTTP/1.1", "", 0, 0x0a000002, 9);
    claimMetadata.contentType = "application/json";
    claimMetadata.contentTypeCount = 1;
    const char claimJson[] = "{\"challenge\":\"01020304\"}";
    TextBody claimBody(claimJson);
    LocalHttpStreamingRequest claim = makeStreamingRequest(
        claimMetadata.requestLine, strlen(claimJson), 9, claimMetadata);
    char responseBody[256];
    LocalHttpResponse response = handler.handle(
        claim, claimBody, responseBody, sizeof(responseBody));
    expect(strcmp(response.status, "201 Created") == 0 &&
               strstr(responseBody,
                      "05060708090a0b0c0d0e0f1011121314") != NULL,
           "session route consumes the displayed challenge exactly once");

    LocalHttpRequest badUpload = makeRequest(
        "POST /api/ota HTTP/1.1",
        "OTA 05060708090A0B0C0D0E0F1011121314", 1,
        0x0a000002, 9);
    badUpload.contentType = "application/octet-stream";
    badUpload.contentTypeCount = 1;
    FakeBody unread(385);
    LocalHttpStreamingRequest upload =
        makeStreamingRequest(badUpload.requestLine, 385, 9, badUpload);
    response = handler.handle(
        upload, unread, responseBody, sizeof(responseBody));
    expect(strcmp(response.status, "401 Unauthorized") == 0 &&
               unread.reads == 0 && response.allowUnreadRequestBody,
           "malformed upload authorization fails before body reads");

    LocalHttpRequest cancel = makeRequest(
        "DELETE /api/ota HTTP/1.1",
        "OTA 05060708090a0b0c0d0e0f1011121314", 1,
        0x0a000002, 9);
    response = handler.handleRequest(cancel, responseBody, sizeof(responseBody));
    expect(strcmp(response.status, "202 Accepted") == 0 &&
               waitForState(controller, LOCAL_OTA_IDLE),
           "authorized DELETE route cancels and invalidates the capability");
}

void testExpiryAndNetworkGenerationCancelReadyWorker() {
    fakeNow = 0;
    FakeCore core;
    FakeEntropy entropy;
    FakeDisplay display;
    NetworkMaintenanceCoordinator network;
    uint8_t key[] = {1};
    LocalOtaController controller(
        core, entropy, display, network, key, sizeof(key),
        "HomeTemperature", "MXCHIP_AZ3166", "1.0.0", readClock);
    controller.begin();
    controller.openChallenge();
    char capability[33];
    controller.claim("01020304", 0x0a000001, 4, capability);
    char authorization[37];
    snprintf(authorization, sizeof(authorization), "OTA %s", capability);
    LocalHttpRequest valid = makeRequest(
        "POST /api/ota HTTP/1.1", authorization, 1, 0x0a000001, 4);
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

    entropy.next = 1;
    controller.openChallenge();
    controller.claim("01020304", 0x0a000001, 5, capability);
    valid = makeRequest("GET /api/ota/status HTTP/1.1",
                        authorization, 1, 0x0a000001, 5);
    fakeNow = LocalOtaController::CAPABILITY_WINDOW_MS + 1;
    controller.update(5);
    uint32_t generation;
    expect(!controller.authorize(valid, generation),
           "expired capability cannot authorize another request");
}

void setup() {
    Serial.begin(115200);
    while (!Serial);
    delay(3000);
    Serial.println("TEST_SUITE: LocalOtaControllerTests");
    testFailClosedConfigurationAndEntropy();
    testAuthorizationLeaseUploadRoutesAndApply();
    testWaitingLeaseAndIdleReadCancellation();
    testFinalApplyRevalidationRaces();
    testTypedErrorsAndRebootCloudPolicy();
    testActivatedAwaitingResponseShutdown();
    testShutdownReleasesPreActivationReservations();
    testUploadVerificationHandoffInvalidation();
    testFatalActivationShutdownRetainsReservation();
    testApplyTimeoutBeforeWorkerValidationPreservesFatal();
    testApplyTimeoutDuringActivatePreservesFatal();
    testClaimAndCancelHttpRoutes();
    testExpiryAndNetworkGenerationCancelReadyWorker();
}

void loop() {
    Serial.print("TEST_RESULT: ");
    Serial.println(failureCount == 0 ? "PASS" : "FAIL");
    delay(1000);
}
