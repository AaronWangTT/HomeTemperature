#include <Arduino.h>

#include "AppConfig.h"
#include "ConnectivityManager.h"

uint32_t fakeNow = 0;
uint32_t fakeConnectDuration = 0;
uint32_t fakeSyncDuration = 0;
bool fakeWiFiConnected = false;
bool fakeConnectResult = false;
bool fakeTimeSynchronized = false;
bool fakeSyncResult = false;
int connectCallCount = 0;
int disconnectCallCount = 0;
int synchronizeCallCount = 0;
int failureCount = 0;

uint32_t getFakeTime() {
    return fakeNow;
}

bool getFakeWiFiConnected() {
    return fakeWiFiConnected;
}

void disconnectFakeWiFi() {
    ++disconnectCallCount;
    fakeWiFiConnected = false;
}

bool connectFakeWiFi() {
    ++connectCallCount;
    fakeNow += fakeConnectDuration;
    fakeWiFiConnected = fakeConnectResult;
    return fakeConnectResult;
}

bool getFakeTimeSynchronized() {
    return fakeTimeSynchronized;
}

void synchronizeFakeTime() {
    ++synchronizeCallCount;
    fakeNow += fakeSyncDuration;
    fakeTimeSynchronized = fakeSyncResult;
}

ConnectivityOperations createFakeOperations() {
    ConnectivityOperations operations = {
        getFakeTime,
        getFakeWiFiConnected,
        disconnectFakeWiFi,
        connectFakeWiFi,
        getFakeTimeSynchronized,
        synchronizeFakeTime
    };
    return operations;
}

ConnectivityManager createManager() {
    ConnectivityOperations operations = createFakeOperations();
    return ConnectivityManager(
        AppConfig::WIFI_STATUS_INTERVAL_MS,
        AppConfig::WIFI_RETRY_INITIAL_MS,
        AppConfig::WIFI_RETRY_MAX_MS,
        AppConfig::NTP_RETRY_INTERVAL_MS,
        operations);
}

void resetFakePlatform() {
    fakeNow = 0;
    fakeConnectDuration = 0;
    fakeSyncDuration = 0;
    fakeWiFiConnected = false;
    fakeConnectResult = false;
    fakeTimeSynchronized = false;
    fakeSyncResult = false;
    connectCallCount = 0;
    disconnectCallCount = 0;
    synchronizeCallCount = 0;
}

void expect(bool condition, const char *name) {
    Serial.print(condition ? "PASS: " : "FAIL: ");
    Serial.println(name);
    if (!condition) {
        ++failureCount;
    }
}

void expectNoEvents(const ConnectivityEvents &events, const char *name) {
    expect(
        !events.wifiConnected &&
            !events.wifiDisconnected &&
            !events.timeSynchronized,
        name);
}

void testRetryDelayPolicy() {
    expect(
        ConnectivityManager::nextRetryDelay(0, 5000, 60000) == 5000,
        "first Wi-Fi retry waits five seconds");
    expect(
        ConnectivityManager::nextRetryDelay(5000, 5000, 60000) == 10000,
        "second Wi-Fi retry waits ten seconds");
    expect(
        ConnectivityManager::nextRetryDelay(10000, 5000, 60000) == 20000,
        "third Wi-Fi retry waits twenty seconds");
    expect(
        ConnectivityManager::nextRetryDelay(20000, 5000, 60000) == 40000,
        "fourth Wi-Fi retry waits forty seconds");
    expect(
        ConnectivityManager::nextRetryDelay(40000, 5000, 60000) == 60000,
        "Wi-Fi retry delay is capped at sixty seconds");
    expect(
        ConnectivityManager::nextRetryDelay(60000, 5000, 60000) == 60000,
        "maximum Wi-Fi retry delay remains capped");
}

void testSuccessfulStartup() {
    resetFakePlatform();
    fakeConnectResult = true;
    fakeTimeSynchronized = true;
    ConnectivityManager manager = createManager();

    ConnectivityEvents events = manager.update();
    expect(events.wifiConnected, "successful startup emits connected event");
    expect(!events.wifiDisconnected,
           "successful startup emits no disconnected event");
    expect(events.timeSynchronized,
           "already synchronized startup emits time event");
    expect(manager.isWiFiConnected(),
           "manager records successful Wi-Fi connection");
    expect(manager.isTimeSynchronized(),
           "manager records synchronized system time");
    expect(connectCallCount == 1,
           "startup performs one Wi-Fi connection attempt");
    expect(disconnectCallCount == 1,
           "startup clears the previous Wi-Fi state once");

    fakeNow = 999;
    expectNoEvents(manager.update(),
                   "connected status is not checked before one second");
    expect(connectCallCount == 1,
           "connected update performs no extra connection attempt");
}

void testConnectionFailureBackoff() {
    resetFakePlatform();
    ConnectivityManager manager = createManager();

    expectNoEvents(manager.update(),
                   "failed startup emits no connection events");
    expect(connectCallCount == 1,
           "failed startup performs one connection attempt");

    fakeNow = 4999;
    manager.update();
    expect(connectCallCount == 1,
           "first retry does not occur before five seconds");

    fakeNow = 5000;
    manager.update();
    expect(connectCallCount == 2,
           "first retry occurs at five seconds");

    fakeNow = 14999;
    manager.update();
    expect(connectCallCount == 2,
           "second retry does not occur before ten more seconds");

    fakeNow = 15000;
    fakeConnectResult = true;
    ConnectivityEvents events = manager.update();
    expect(connectCallCount == 3,
           "second retry occurs after ten more seconds");
    expect(events.wifiConnected,
           "successful retry emits connected event");
}

void testRetryStartsAfterBlockingAttempt() {
    resetFakePlatform();
    fakeNow = 100;
    fakeConnectDuration = 2000;
    ConnectivityManager manager = createManager();

    manager.update();
    expect(fakeNow == 2100,
           "fake connection attempt advances the platform clock");

    fakeConnectDuration = 0;
    fakeNow = 7099;
    manager.update();
    expect(connectCallCount == 1,
           "retry delay is measured from connection attempt completion");

    fakeNow = 7100;
    manager.update();
    expect(connectCallCount == 2,
           "retry occurs five seconds after attempt completion");
}

void testDisconnectAndReconnectEvents() {
    resetFakePlatform();
    fakeConnectResult = true;
    fakeTimeSynchronized = true;
    ConnectivityManager manager = createManager();
    manager.update();

    fakeWiFiConnected = false;
    fakeNow = 999;
    expectNoEvents(manager.update(),
                   "connection loss waits for the status interval");

    fakeNow = 1000;
    ConnectivityEvents lost = manager.update();
    expect(lost.wifiDisconnected,
           "connection loss emits disconnected event");
    expect(!lost.wifiConnected,
           "loss update does not reconnect before application cleanup");
    expect(!manager.isWiFiConnected(),
           "manager clears Wi-Fi state after connection loss");
    expect(!manager.isTimeSynchronized(),
           "manager clears time state after connection loss");
    expect(connectCallCount == 1,
           "connection loss does not reconnect in the same update");

    fakeNow = 1001;
    ConnectivityEvents reconnected = manager.update();
    expect(reconnected.wifiConnected,
           "next update reconnects immediately after loss");
    expect(connectCallCount == 2,
           "reconnection performs one new connection attempt");
}

void testNtpRetry() {
    resetFakePlatform();
    fakeConnectResult = true;
    ConnectivityManager manager = createManager();
    manager.update();

    expect(!manager.isTimeSynchronized(),
           "manager records unsynchronized startup time");
    expect(synchronizeCallCount == 0,
           "NTP is not retried immediately after connection");

    fakeNow = 59999;
    manager.update();
    expect(synchronizeCallCount == 0,
           "NTP is not retried before sixty seconds");

    fakeNow = 60000;
    ConnectivityEvents failed = manager.update();
    expect(synchronizeCallCount == 1,
           "NTP is retried at sixty seconds");
    expect(!failed.timeSynchronized,
           "failed NTP retry emits no synchronized event");

    fakeNow = 119999;
    manager.update();
    expect(synchronizeCallCount == 1,
           "second NTP retry waits another sixty seconds");

    fakeNow = 120000;
    fakeSyncResult = true;
    ConnectivityEvents synchronized = manager.update();
    expect(synchronizeCallCount == 2,
           "second NTP retry occurs after sixty more seconds");
    expect(synchronized.timeSynchronized,
           "successful NTP retry emits synchronized event");
    expect(manager.isTimeSynchronized(),
           "manager records successful NTP synchronization");
}

void testStatusMillisWraparound() {
    resetFakePlatform();
    fakeNow = 0xFFFFFF00UL;
    fakeConnectResult = true;
    fakeTimeSynchronized = true;
    ConnectivityManager manager = createManager();
    manager.update();

    fakeWiFiConnected = false;
    fakeNow = 0xFFFFFF00UL + 999UL;
    expectNoEvents(manager.update(),
                   "wrapped status interval waits until one second");

    fakeNow = 0xFFFFFF00UL + 1000UL;
    ConnectivityEvents events = manager.update();
    expect(events.wifiDisconnected,
           "wrapped status interval detects loss on time");
}

void testRetryMillisWraparound() {
    resetFakePlatform();
    fakeNow = 0xFFFFFF00UL;
    ConnectivityManager manager = createManager();
    manager.update();

    fakeNow = 0xFFFFFF00UL + 4999UL;
    manager.update();
    expect(connectCallCount == 1,
           "wrapped retry interval is not due one millisecond early");

    fakeNow = 0xFFFFFF00UL + 5000UL;
    manager.update();
    expect(connectCallCount == 2,
           "wrapped retry interval is due on time");
}

void setup() {
    Serial.begin(115200);
    while (!Serial);
    delay(3000);

    Serial.println("TEST_SUITE: ConnectivityManagerTests");
    testRetryDelayPolicy();
    testSuccessfulStartup();
    testConnectionFailureBackoff();
    testRetryStartsAfterBlockingAttempt();
    testDisconnectAndReconnectEvents();
    testNtpRetry();
    testStatusMillisWraparound();
    testRetryMillisWraparound();
}

void loop() {
    Serial.print("TEST_RESULT: ");
    Serial.println(failureCount == 0 ? "PASS" : "FAIL");
    delay(1000);
}