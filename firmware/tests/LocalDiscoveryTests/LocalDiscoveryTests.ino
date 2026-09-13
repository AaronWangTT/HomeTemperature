#include <Arduino.h>
#include <string.h>
#include <type_traits>
#include "mbed_stats.h"

#include <ArduinoMDNS.h>
#include "src/discovery/LocalDiscovery.h"
#include "src/discovery/Az3166LocalDiscoveryOperations.h"
#include "src/discovery/MdnsUdpTransport.h"

static_assert(std::is_abstract<LocalDiscoveryOperations>::value,
              "LocalDiscoveryOperations must remain an interface");
static_assert(std::has_virtual_destructor<LocalDiscoveryOperations>::value,
              "LocalDiscoveryOperations must have a virtual destructor");
static_assert(!std::is_copy_constructible<LocalDiscovery>::value &&
                  !std::is_copy_assignable<LocalDiscovery>::value,
              "LocalDiscovery must not copy an active session");
static_assert(!std::is_move_constructible<LocalDiscovery>::value &&
                  !std::is_move_assignable<LocalDiscovery>::value,
              "LocalDiscovery must remain at its callback-bound address");

class FakeLocalDiscoveryOperations : public LocalDiscoveryOperations {
public:
    uint32_t fakeNow = 0;
    uint32_t fakeStartDuration = 0;
    uint32_t lastStartedAddress = 0;
    LocalDiscoveryService lastStartedService = {};
    int startCount = 0;
    int stopCount = 0;
    bool fakeStartResult = true;
    bool fakeHealthy = true;

    uint32_t currentTime() override { return fakeNow; }

    bool start(uint32_t address, const LocalDiscoveryService &service) override {
        ++startCount;
        lastStartedAddress = address;
        lastStartedService = service;
        fakeNow += fakeStartDuration;
        return fakeStartResult;
    }

    void stop() override { ++stopCount; }
    bool isHealthy() override { return fakeHealthy; }
};

int failureCount = 0;

const LocalDiscoveryService TEST_SERVICE = {
    "az3166", "az3166._http", 80, "\x13" "path=/api/telemetry"
};

void expect(bool condition, const char *name) {
    Serial.print(condition ? "PASS: " : "FAIL: ");
    Serial.println(name);
    if (!condition) {
        ++failureCount;
    }
}

class NativeStartupFailureOperations : public Az3166LocalDiscoveryOperations {
public:
    NativeStartupFailureOperations()
        : probe_(osPriorityAboveNormal, 2048), completed_(0), probeStarted_(false) {
    }

    ~NativeStartupFailureOperations() override {
        finishProbe();
    }

    bool configureAllowed = false;
    bool resourcesConfigured = false;
    bool setupCompleteAtStart = false;
    bool startupWindowAccessible = false;
    bool earlyIterationGated = false;
    int configureCount = 0;
    int startCount = 0;
    int cleanupCount = 0;
    int serviceCount = 0;

    bool finishProbe() {
        if (!probeStarted_) {
            return false;
        }
        osStatus result = probe_.join();
        probeStarted_ = false;
        return result == osOK;
    }

    bool pollOnce() {
        return serviceOnce();
    }

protected:
    bool configureResponder(uint32_t, const LocalDiscoveryService &) override {
        ++configureCount;
        resourcesConfigured = true;
        return configureAllowed;
    }

    void endResponder() override {
        ++cleanupCount;
        resourcesConfigured = false;
        Az3166LocalDiscoveryOperations::endResponder();
    }

    bool serviceResponder() override {
        ++serviceCount;
        return true;
    }

    osStatus startWorker() override {
        ++startCount;
        setupCompleteAtStart = resourcesConfigured;
        probeStarted_ = probe_.start(mbed::callback(
            this, &NativeStartupFailureOperations::probeOnce)) == osOK;
        if (probeStarted_) {
            startupWindowAccessible = completed_.wait(1000) > 0;
        }
        return osErrorResource;
    }

private:
    void probeOnce() {
        bool keepRunning = serviceOnce();
        earlyIterationGated = keepRunning && !isHealthy() && serviceCount == 0;
        completed_.release();
    }

    rtos::Thread probe_;
    rtos::Semaphore completed_;
    bool probeStarted_;
};

void testNativeStartupFailure() {
    static NativeStartupFailureOperations operations;
    expect(!operations.start(0xC0000201UL, TEST_SERVICE) &&
               operations.configureCount == 1 && operations.startCount == 0 &&
               operations.cleanupCount == 1 && !operations.resourcesConfigured &&
               !operations.isHealthy(),
           "native setup failure cleans partial state without starting a worker");

    operations.configureAllowed = true;
    bool started = operations.start(0xC0000201UL, TEST_SERVICE);
    bool joined = operations.finishProbe();
    expect(joined && operations.startupWindowAccessible && operations.setupCompleteAtStart,
           "native worker startup releases the responder mutex after setup");
    expect(operations.earlyIterationGated && operations.serviceCount == 0,
           "an early native worker iteration cannot use the responder before readiness");
    expect(!started && operations.configureCount == 2 && operations.startCount == 1 &&
               operations.cleanupCount == 2 && !operations.resourcesConfigured &&
               !operations.isHealthy(),
           "native thread-start failure releases responder state and remains unhealthy");
    expect(operations.pollOnce() && operations.serviceCount == 0,
           "native worker iterations remain gated after thread-start failure");
}

void testScopeCleanup() {
    FakeLocalDiscoveryOperations operations;
    {
        LocalDiscovery discovery(5000, TEST_SERVICE, operations);
    }
    expect(operations.startCount == 0 && operations.stopCount == 0,
           "an unused controller does not stop its backend");

    {
        LocalDiscovery discovery(5000, TEST_SERVICE, operations);
        discovery.update(true, 0xC0000201UL);
    }
    expect(operations.startCount == 1 && operations.stopCount == 1,
           "scope exit stops an active session exactly once");

    {
        LocalDiscovery discovery(5000, TEST_SERVICE, operations);
        discovery.update(true, 0xC0000201UL);
        discovery.update(false, 0);
    }
    expect(operations.startCount == 2 && operations.stopCount == 2,
           "destruction does not repeat explicit session cleanup");

    operations.fakeStartResult = false;
    {
        LocalDiscovery discovery(5000, TEST_SERVICE, operations);
        discovery.update(true, 0xC0000201UL);
    }
    expect(operations.startCount == 3 && operations.stopCount == 3,
           "failed startup is cleaned once without a second destructor stop");
}

void testSharedBackendOwnership() {
    FakeLocalDiscoveryOperations operations;
    {
        LocalDiscovery waiting(5000, TEST_SERVICE, operations);
        {
            LocalDiscovery owner(5000, TEST_SERVICE, operations);
            owner.update(true, 0xC0000201UL);
            waiting.update(true, 0xC0000202UL);
            expect(owner.isRunning() && !waiting.isRunning() &&
                       operations.startCount == 1 && operations.stopCount == 0 &&
                       operations.lastStartedAddress == 0xC0000201UL,
                   "a busy backend cannot be restarted by another controller");

            {
                LocalDiscovery rejected(5000, TEST_SERVICE, operations);
                rejected.update(true, 0xC0000203UL);
                rejected.update(false, 0);
            }
            expect(owner.isRunning() && operations.stopCount == 0,
                   "a non-owner cannot stop the active session on disconnect or destruction");

            operations.fakeNow = 4999;
            waiting.update(true, 0xC0000202UL);
            expect(operations.startCount == 1,
                   "a waiting controller respects the retry interval");
        }
        expect(operations.stopCount == 1,
               "owner destruction releases the shared backend");
        operations.fakeNow = 5000;
        waiting.update(true, 0xC0000202UL);
        expect(waiting.isRunning() && operations.startCount == 2 &&
                   operations.lastStartedAddress == 0xC0000202UL,
               "a waiting controller acquires the released backend on retry");
    }
    expect(operations.stopCount == 2,
           "each shared-backend session is stopped by its own owner once");
}

void testFailedStartupReleasesOwnership() {
    FakeLocalDiscoveryOperations operations;
    operations.fakeStartResult = false;
    LocalDiscovery failed(5000, TEST_SERVICE, operations);
    failed.update(true, 0xC0000201UL);

    operations.fakeStartResult = true;
    {
        LocalDiscovery replacement(5000, TEST_SERVICE, operations);
        replacement.update(true, 0xC0000202UL);
        expect(replacement.isRunning() && operations.startCount == 2 &&
                   operations.stopCount == 1,
               "failed startup releases its lease for a different controller");
        failed.update(false, 0);
        expect(replacement.isRunning() && operations.stopCount == 1,
               "the failed controller cannot clean up the replacement session");
    }
    expect(operations.stopCount == 2,
           "the replacement session is released on scope exit");
}

void testConnectionLifecycle() {
    FakeLocalDiscoveryOperations operations;
    LocalDiscovery discovery(5000, TEST_SERVICE, operations);
    discovery.update(false, 0xC0000201UL);
    discovery.update(true, 0);
    expect(operations.startCount == 0 && !discovery.isRunning(),
           "discovery waits for Wi-Fi and an assigned address");
    discovery.update(true, 0xC0000201UL);
    expect(operations.startCount == 1 && discovery.isRunning(),
           "delayed address acquisition starts discovery");
    discovery.update(true, 0xC0000201UL);
    expect(operations.startCount == 1 && operations.stopCount == 0,
           "unchanged connectivity does not restart discovery");
    discovery.update(true, 0xC0000202UL);
    expect(operations.startCount == 2 && operations.stopCount == 1 &&
               operations.lastStartedAddress == 0xC0000202UL,
           "address changes replace the advertised address");
    discovery.update(false, 0xC0000202UL);
    discovery.update(false, 0xC0000202UL);
    expect(operations.stopCount == 2 && !discovery.isRunning(),
           "disconnect stops discovery exactly once");
    discovery.update(true, 0xC0000202UL);
    expect(operations.startCount == 3 && discovery.isRunning(),
           "same-address reconnect starts fresh discovery");
    discovery.update(true, 0);
    expect(operations.stopCount == 3 && !discovery.isRunning(),
           "address loss stops discovery without Wi-Fi loss");
}

void testServiceConfiguration() {
    FakeLocalDiscoveryOperations operations;
    LocalDiscoveryService service = {
        "example", "example._http", 8080, "\x0c" "path=/sample"
    };
    LocalDiscovery discovery(5000, service, operations);
    discovery.update(true, 0xC0000201UL);
    expect(operations.lastStartedService.port == 8080 &&
               strcmp(operations.lastStartedService.hostname, "example") == 0 &&
               strcmp(operations.lastStartedService.serviceName, "example._http") == 0 &&
               strcmp(operations.lastStartedService.txtRecord, "\x0c" "path=/sample") == 0,
           "discovery forwards caller-supplied hostname, port, and service metadata");
}

void testStartFailureBackoff() {
    FakeLocalDiscoveryOperations operations;
    operations.fakeStartResult = false;
    operations.fakeStartDuration = 100;
    LocalDiscovery discovery(5000, TEST_SERVICE, operations);
    discovery.update(true, 0xC0000201UL);
    expect(!discovery.isRunning() && operations.stopCount == 1,
           "failed startup releases partial responder state");
    operations.fakeNow = 5099;
    discovery.update(true, 0xC0000201UL);
    expect(operations.startCount == 1, "retry waits from startup completion");
    operations.fakeNow = 5100;
    operations.fakeStartResult = true;
    discovery.update(true, 0xC0000201UL);
    expect(operations.startCount == 2 && discovery.isRunning(),
           "startup retry succeeds at the configured interval");
}

void testTransportFailure() {
    FakeLocalDiscoveryOperations operations;
    LocalDiscovery discovery(5000, TEST_SERVICE, operations);
    discovery.update(true, 0xC0000201UL);
    operations.fakeHealthy = false;
    operations.fakeNow = 100;
    discovery.update(true, 0xC0000201UL);
    expect(!discovery.isRunning() && operations.stopCount == 1,
           "worker transport failure stops discovery");
    operations.fakeNow = 5099;
    discovery.update(true, 0xC0000201UL);
    expect(operations.startCount == 1 && operations.stopCount == 1,
           "transport failure does not create a retry loop");
    operations.fakeHealthy = true;
    operations.fakeNow = 5100;
    discovery.update(true, 0xC0000201UL);
    expect(operations.startCount == 2 && discovery.isRunning(),
           "transport failure recovers after the retry interval");
}

void testRetryAddressChangeAndWraparound() {
    FakeLocalDiscoveryOperations operations;
    operations.fakeStartResult = false;
    operations.fakeNow = 0xFFFFFF00UL;
    LocalDiscovery discovery(5000, TEST_SERVICE, operations);
    discovery.update(true, 0xC0000201UL);
    operations.fakeNow = 0xFFFFFF00UL + 4999UL;
    discovery.update(true, 0xC0000201UL);
    expect(operations.startCount == 1, "retry interval is wraparound safe");
    operations.fakeNow = 0xFFFFFF00UL + 5000UL;
    discovery.update(true, 0xC0000201UL);
    expect(operations.startCount == 2, "wrapped retry occurs on time");
    operations.fakeStartResult = true;
    discovery.update(true, 0xC0000202UL);
    expect(operations.startCount == 3 && operations.lastStartedAddress == 0xC0000202UL,
           "a new address bypasses the old address retry delay");
}

void testIndependentOperations() {
    FakeLocalDiscoveryOperations firstOperations;
    FakeLocalDiscoveryOperations secondOperations;
    secondOperations.fakeStartResult = false;
    LocalDiscovery firstDiscovery(5000, TEST_SERVICE, firstOperations);
    LocalDiscovery secondDiscovery(5000, TEST_SERVICE, secondOperations);

    firstDiscovery.update(true, 0xC0000201UL);
    secondDiscovery.update(true, 0xC0000202UL);
    expect(firstDiscovery.isRunning() && !secondDiscovery.isRunning() &&
               firstOperations.stopCount == 0 && secondOperations.stopCount == 1,
           "discovery instances keep backend results and cleanup separate");

    secondOperations.fakeNow = 5000;
    secondOperations.fakeStartResult = true;
    secondDiscovery.update(true, 0xC0000202UL);
    expect(secondDiscovery.isRunning() && firstOperations.fakeNow == 0 &&
               firstOperations.startCount == 1 && secondOperations.startCount == 2,
           "one backend's retry clock does not affect another instance");

    firstOperations.fakeHealthy = false;
    firstDiscovery.update(true, 0xC0000201UL);
    expect(!firstDiscovery.isRunning() && secondDiscovery.isRunning() &&
               firstOperations.lastStartedAddress == 0xC0000201UL &&
               secondOperations.lastStartedAddress == 0xC0000202UL,
           "health and advertised addresses belong to the injected backend");
}

class CaptureTransport {
public:
    uint8_t output[512];
    size_t outputLength;
    int sentCount;
    bool sendAllowed;
    uint8_t input[128];
    size_t inputLength;
    bool shortRead;

    CaptureTransport()
        : outputLength(0), sentCount(0), sendAllowed(true), inputLength(0),
          shortRead(false) {}

    uint8_t beginMulticast(IPAddress address, uint16_t port) {
        return address == IPAddress(224, 0, 0, 251) && port == 5353;
    }
    void stop() { inputLength = 0; }
    int beginPacket(IPAddress address, uint16_t port) {
        outputLength = 0;
        return address == IPAddress(224, 0, 0, 251) && port == 5353;
    }
    size_t write(const uint8_t *buffer, size_t size) {
        if (size > sizeof(output) - outputLength) {
            return 0;
        }
        memcpy(output + outputLength, buffer, size);
        outputLength += size;
        return size;
    }
    int endPacket() {
        ++sentCount;
        return sendAllowed;
    }
    int parsePacket() { return static_cast<int>(inputLength); }
    int read(uint8_t *buffer, size_t size) {
        if (size > inputLength) {
            size = inputLength;
        }
        if (shortRead && size > 0) {
            --size;
        }
        memcpy(buffer, input, size);
        inputLength = 0;
        return static_cast<int>(size);
    }
    void flush() { inputLength = 0; }
    IPAddress remoteIP() { return IPAddress(192, 0, 2, 10); }
    uint16_t remotePort() { return 5353; }

    void queue(const uint8_t *buffer, size_t size) {
        memcpy(input, buffer, size);
        inputLength = size;
        sentCount = 0;
    }
};

const uint8_t ADDRESS_QUERY[] = {
    0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
    6, 'a', 'z', '3', '1', '6', '6',
    5, 'l', 'o', 'c', 'a', 'l', 0,
    0, 1, 0, 1
};

bool containsBytes(const uint8_t *buffer, size_t size, const char *text) {
    size_t length = strlen(text);
    for (size_t offset = 0; offset + length <= size; ++offset) {
        if (memcmp(buffer + offset, text, length) == 0) {
            return true;
        }
    }
    return false;
}

void testHostnameAnswers() {
    CaptureTransport transport;
    MDNS responder(transport, false);
    expect(responder.begin(IPAddress(192, 0, 2, 1), "az3166") == 1,
           "responder joins the standard mDNS group");
    transport.queue(ADDRESS_QUERY, sizeof(ADDRESS_QUERY));
    responder.run();
    expect(transport.sentCount == 1 && transport.outputLength == 40,
           "az3166.local A query receives one complete answer");
    expect(transport.output[28] == 0x80 && transport.output[29] == 1 &&
               transport.output[33] == 120 && transport.output[35] == 4,
           "address answer has cache-flush and a 120-second TTL");
    expect(transport.output[36] == 192 && transport.output[37] == 0 &&
               transport.output[38] == 2 && transport.output[39] == 1,
           "address answer contains the current IPv4 address");

    transport.queue(ADDRESS_QUERY, sizeof(ADDRESS_QUERY));
    transport.input[13] = 'A';
    transport.input[14] = 'Z';
    responder.run();
    expect(transport.sentCount == 1, "hostname matching ignores case");

    transport.queue(ADDRESS_QUERY, sizeof(ADDRESS_QUERY));
    transport.input[27] = 0x1c;
    responder.run();
    expect(transport.sentCount == 1 && (transport.output[3] & 0x0f) == 0,
           "AAAA query does not claim the IPv4 hostname is nonexistent");
}

void testMalformedQueries() {
    CaptureTransport transport;
    MDNS responder(transport, false);
    responder.begin(IPAddress(192, 0, 2, 1), "az3166");
    transport.queue(ADDRESS_QUERY, 5);
    responder.run();
    expect(transport.sentCount == 0, "truncated DNS header is ignored");
    transport.queue(ADDRESS_QUERY, sizeof(ADDRESS_QUERY) - 1);
    responder.run();
    expect(transport.sentCount == 0, "truncated DNS question is ignored");
    transport.queue(ADDRESS_QUERY, sizeof(ADDRESS_QUERY));
    transport.input[12] = 63;
    responder.run();
    expect(transport.sentCount == 0, "out-of-packet label is ignored");

    uint8_t longName[58] = {};
    longName[5] = 1;
    longName[12] = 40;
    memset(longName + 13, 'x', 40);
    longName[55] = longName[57] = 1;
    transport.queue(longName, sizeof(longName));
    responder.run();
    expect(transport.sentCount == 0,
           "long unrelated names do not overrun hostname comparisons");
}

void testMalformedQueryHeapCleanup() {
    CaptureTransport transport;
    MDNS responder(transport, false);
    if (responder.begin(IPAddress(192, 0, 2, 1), "az3166") != 1) {
     expect(false, "heap regression initializes the responder");
     return;
    }

    mbed_stats_heap_t before = {};
    mbed_stats_heap_t after = {};
    mbed_stats_heap_get(&before);
    bool packetsIgnored = true;
    for (int attempt = 0; attempt < 32; ++attempt) {
     transport.queue(ADDRESS_QUERY, 5);
     responder.run();
     packetsIgnored &= transport.sentCount == 0;

     transport.queue(ADDRESS_QUERY, 12);
     responder.run();
     packetsIgnored &= transport.sentCount == 0;

     transport.queue(ADDRESS_QUERY, sizeof(ADDRESS_QUERY) - 1);
     responder.run();
     packetsIgnored &= transport.sentCount == 0;

     transport.queue(ADDRESS_QUERY, 13);
     transport.input[12] = 0xc0;
     responder.run();
     packetsIgnored &= transport.sentCount == 0;

     transport.queue(ADDRESS_QUERY, sizeof(ADDRESS_QUERY));
     transport.input[12] = 63;
     responder.run();
     packetsIgnored &= transport.sentCount == 0;

     transport.queue(ADDRESS_QUERY, sizeof(ADDRESS_QUERY));
     transport.shortRead = true;
     responder.run();
     transport.shortRead = false;
     packetsIgnored &= transport.sentCount == 0;
    }
    mbed_stats_heap_get(&after);

    expect(after.total_size > before.total_size,
        "heap statistics observe the packet buffer allocations");
    expect(after.current_size == before.current_size && after.alloc_cnt == before.alloc_cnt,
        "repeated malformed and short-read packets retain no heap allocations");
    expect(after.alloc_fail_cnt == before.alloc_fail_cnt && packetsIgnored,
        "malformed packet bursts are rejected without allocation failures");

    transport.queue(ADDRESS_QUERY, sizeof(ADDRESS_QUERY));
    responder.run();
    expect(transport.sentCount == 1 && transport.outputLength == 40,
        "valid queries still receive an answer after malformed packet bursts");
}

void testServiceAndCleanup() {
    CaptureTransport transport;
    MDNS responder(transport, false);
    bool restarted = true;
    for (int attempt = 0; attempt < 20; ++attempt) {
        restarted &= responder.begin(IPAddress(192, 0, 2, 1), "az3166") == 1;
        restarted &= responder.addServiceRecord(
            "az3166._http", 80, MDNSServiceTCP,
            "\x13" "path=/api/telemetry") == 1;
        responder.end();
    }
    expect(restarted, "repeated begin/end releases service record slots");
    responder.begin(IPAddress(192, 0, 2, 2), "az3166");
    expect(responder.addServiceRecord(
               "az3166._http", 80, MDNSServiceTCP,
               "\x13" "path=/api/telemetry") == 1,
           "HTTP service registration sends an announcement");
    expect(containsBytes(transport.output, transport.outputLength, "_http") &&
               containsBytes(transport.output, transport.outputLength,
                             "\x13" "path=/api/telemetry"),
           "service announcement contains HTTP and the telemetry TXT path");
    expect(transport.output[transport.outputLength - 1] == 2,
           "service announcement uses the replacement IPv4 address");
    transport.sendAllowed = false;
    expect(responder.announce() == 0,
           "send failures are reported to the discovery backend");
}

void testTransportBounds() {
    MdnsUdpTransport transport;
    expect(transport.beginMulticast(IPAddress(224, 0, 0, 251), 5353) == 0,
           "native transport rejects startup without a local address");
    uint8_t oversized[513] = {};
    expect(transport.write(oversized, sizeof(oversized)) == 0,
           "oversized outbound packets are rejected without truncation");
    expect(transport.endPacket() == 0,
           "closed transport does not report a successful send");
    transport.stop();
    expect(!transport.failed() && transport.parsePacket() == 0,
           "transport cleanup clears failures and receive state");
}

void setup() {
    Serial.begin(115200);
    while (!Serial);
    delay(3000);
    Serial.println("TEST_SUITE: LocalDiscoveryTests");
    testNativeStartupFailure();
    testScopeCleanup();
    testSharedBackendOwnership();
    testFailedStartupReleasesOwnership();
    testIndependentOperations();
    testConnectionLifecycle();
    testServiceConfiguration();
    testStartFailureBackoff();
    testTransportFailure();
    testRetryAddressChangeAndWraparound();
    testHostnameAnswers();
    testMalformedQueries();
    testMalformedQueryHeapCleanup();
    testServiceAndCleanup();
    testTransportBounds();
}

void loop() {
    Serial.print("TEST_RESULT: ");
    Serial.println(failureCount == 0 ? "PASS" : "FAIL");
    delay(1000);
}