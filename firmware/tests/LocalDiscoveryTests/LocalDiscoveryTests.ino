#include <Arduino.h>
#include <string.h>

#include "src/LocalDiscovery.h"
#include "src/MdnsUdpTransport.h"
#include "src/mdns/MDNS.h"

uint32_t fakeNow = 0;
uint32_t fakeStartDuration = 0;
uint32_t lastStartedAddress = 0;
int startCount = 0;
int stopCount = 0;
int failureCount = 0;
bool fakeStartResult = false;
bool fakeHealthy = false;

uint32_t currentTime() { return fakeNow; }

bool startDiscovery(uint32_t address) {
    ++startCount;
    lastStartedAddress = address;
    fakeNow += fakeStartDuration;
    return fakeStartResult;
}

void stopDiscovery() { ++stopCount; }
bool isHealthy() { return fakeHealthy; }

LocalDiscovery createDiscovery() {
    LocalDiscoveryOperations operations = {
        currentTime, startDiscovery, stopDiscovery, isHealthy
    };
    return LocalDiscovery(5000, operations);
}

void resetPlatform() {
    fakeNow = fakeStartDuration = lastStartedAddress = 0;
    startCount = stopCount = 0;
    fakeStartResult = fakeHealthy = true;
}

void expect(bool condition, const char *name) {
    Serial.print(condition ? "PASS: " : "FAIL: ");
    Serial.println(name);
    if (!condition) {
        ++failureCount;
    }
}

void testConnectionLifecycle() {
    resetPlatform();
    LocalDiscovery discovery = createDiscovery();
    discovery.update(false, 0xC0000201UL);
    discovery.update(true, 0);
    expect(startCount == 0 && !discovery.isRunning(),
           "discovery waits for Wi-Fi and an assigned address");
    discovery.update(true, 0xC0000201UL);
    expect(startCount == 1 && discovery.isRunning(),
           "delayed address acquisition starts discovery");
    discovery.update(true, 0xC0000201UL);
    expect(startCount == 1 && stopCount == 0,
           "unchanged connectivity does not restart discovery");
    discovery.update(true, 0xC0000202UL);
    expect(startCount == 2 && stopCount == 1 &&
               lastStartedAddress == 0xC0000202UL,
           "address changes replace the advertised address");
    discovery.update(false, 0xC0000202UL);
    discovery.update(false, 0xC0000202UL);
    expect(stopCount == 2 && !discovery.isRunning(),
           "disconnect stops discovery exactly once");
    discovery.update(true, 0xC0000202UL);
    expect(startCount == 3 && discovery.isRunning(),
           "same-address reconnect starts fresh discovery");
    discovery.update(true, 0);
    expect(stopCount == 3 && !discovery.isRunning(),
           "address loss stops discovery without Wi-Fi loss");
}

void testStartFailureBackoff() {
    resetPlatform();
    fakeStartResult = false;
    fakeStartDuration = 100;
    LocalDiscovery discovery = createDiscovery();
    discovery.update(true, 0xC0000201UL);
    expect(!discovery.isRunning() && stopCount == 1,
           "failed startup releases partial responder state");
    fakeNow = 5099;
    discovery.update(true, 0xC0000201UL);
    expect(startCount == 1, "retry waits from startup completion");
    fakeNow = 5100;
    fakeStartResult = true;
    discovery.update(true, 0xC0000201UL);
    expect(startCount == 2 && discovery.isRunning(),
           "startup retry succeeds at the configured interval");
}

void testTransportFailure() {
    resetPlatform();
    LocalDiscovery discovery = createDiscovery();
    discovery.update(true, 0xC0000201UL);
    fakeHealthy = false;
    fakeNow = 100;
    discovery.update(true, 0xC0000201UL);
    expect(!discovery.isRunning() && stopCount == 1,
           "worker transport failure stops discovery");
    fakeNow = 5099;
    discovery.update(true, 0xC0000201UL);
    expect(startCount == 1 && stopCount == 1,
           "transport failure does not create a retry loop");
    fakeHealthy = true;
    fakeNow = 5100;
    discovery.update(true, 0xC0000201UL);
    expect(startCount == 2 && discovery.isRunning(),
           "transport failure recovers after the retry interval");
}

void testRetryAddressChangeAndWraparound() {
    resetPlatform();
    fakeStartResult = false;
    fakeNow = 0xFFFFFF00UL;
    LocalDiscovery discovery = createDiscovery();
    discovery.update(true, 0xC0000201UL);
    fakeNow = 0xFFFFFF00UL + 4999UL;
    discovery.update(true, 0xC0000201UL);
    expect(startCount == 1, "retry interval is wraparound safe");
    fakeNow = 0xFFFFFF00UL + 5000UL;
    discovery.update(true, 0xC0000201UL);
    expect(startCount == 2, "wrapped retry occurs on time");
    fakeStartResult = true;
    discovery.update(true, 0xC0000202UL);
    expect(startCount == 3 && lastStartedAddress == 0xC0000202UL,
           "a new address bypasses the old address retry delay");
}

class CaptureTransport : public MdnsTransport {
public:
    uint8_t output[512];
    size_t outputLength;
    int sentCount;
    bool sendAllowed;
    uint8_t input[128];
    size_t inputLength;

    CaptureTransport()
        : outputLength(0), sentCount(0), sendAllowed(true), inputLength(0) {}

    uint8_t beginMulticast(IPAddress address, uint16_t port) override {
        return address == IPAddress(224, 0, 0, 251) && port == 5353;
    }
    void stop() override { inputLength = 0; }
    int beginPacket(IPAddress address, uint16_t port) override {
        outputLength = 0;
        return address == IPAddress(224, 0, 0, 251) && port == 5353;
    }
    size_t write(const uint8_t *buffer, size_t size) override {
        if (size > sizeof(output) - outputLength) {
            return 0;
        }
        memcpy(output + outputLength, buffer, size);
        outputLength += size;
        return size;
    }
    int endPacket() override {
        ++sentCount;
        return sendAllowed;
    }
    int parsePacket() override { return static_cast<int>(inputLength); }
    int read(uint8_t *buffer, size_t size) override {
        if (size > inputLength) {
            size = inputLength;
        }
        memcpy(buffer, input, size);
        inputLength = 0;
        return static_cast<int>(size);
    }
    void flush() override { inputLength = 0; }
    IPAddress remoteIP() override { return IPAddress(192, 0, 2, 10); }
    uint16_t remotePort() override { return 5353; }

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
    MDNS responder(transport);
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
    MDNS responder(transport);
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

void testServiceAndCleanup() {
    CaptureTransport transport;
    MDNS responder(transport);
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
    testConnectionLifecycle();
    testStartFailureBackoff();
    testTransportFailure();
    testRetryAddressChangeAndWraparound();
    testHostnameAnswers();
    testMalformedQueries();
    testServiceAndCleanup();
    testTransportBounds();
}

void loop() {
    Serial.print("TEST_RESULT: ");
    Serial.println(failureCount == 0 ? "PASS" : "FAIL");
    delay(1000);
}