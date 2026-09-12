#include <Arduino.h>
#include <type_traits>

#include "src/config/AppConfig.h"
#include "src/connectivity/ConnectivityManager.h"

static_assert(std::is_abstract<ConnectivityOperations>::value,
                       "ConnectivityOperations must remain an interface");
static_assert(std::has_virtual_destructor<ConnectivityOperations>::value,
                       "ConnectivityOperations must have a virtual destructor");

class FakeConnectivityOperations : public ConnectivityOperations {
public:
       uint32_t fakeNow = 0;
       uint32_t fakeConnectDuration = 0;
       uint32_t fakeSyncDuration = 0;
       uint32_t fakeLocalIPv4Address = 0xC0000201UL;
       bool fakeWiFiConnected = false;
       bool fakeConnectResult = false;
       bool fakeTimeSynchronized = false;
       bool fakeSyncResult = false;
       int connectCallCount = 0;
       int disconnectCallCount = 0;
       int synchronizeCallCount = 0;
       int addressReadCallCount = 0;

       uint32_t currentTime() override {
              return fakeNow;
       }

       bool isWiFiConnected() override {
              return fakeWiFiConnected;
       }

       void disconnectWiFi() override {
              ++disconnectCallCount;
              fakeWiFiConnected = false;
       }

       bool connectWiFi() override {
              ++connectCallCount;
              fakeNow += fakeConnectDuration;
              fakeWiFiConnected = fakeConnectResult;
              return fakeConnectResult;
       }

       bool isTimeSynchronized() override {
              return fakeTimeSynchronized;
       }

       void synchronizeTime() override {
              ++synchronizeCallCount;
              fakeNow += fakeSyncDuration;
              fakeTimeSynchronized = fakeSyncResult;
       }

       uint32_t readLocalIPv4Address() override {
              ++addressReadCallCount;
              return fakeLocalIPv4Address;
       }
};

int failureCount = 0;

ConnectivityManager createManager(ConnectivityOperations &operations) {
    return ConnectivityManager(
        AppConfig::WIFI_STATUS_INTERVAL_MS,
        AppConfig::WIFI_RETRY_INITIAL_MS,
        AppConfig::WIFI_RETRY_MAX_MS,
        AppConfig::NTP_RETRY_INTERVAL_MS,
        operations);
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
                     !events.timeSynchronized &&
                     !events.localAddressChanged,
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

void testLocalIPv4AddressParsing() {
    expect(ConnectivityManager::parseLocalIPv4Address(NULL) == 0,
           "null platform address is safely treated as unavailable");
    expect(ConnectivityManager::parseLocalIPv4Address("") == 0,
           "empty platform address is treated as unavailable");
    expect(ConnectivityManager::parseLocalIPv4Address("0.0.0.0") == 0,
           "unassigned IPv4 address is represented as zero");
    expect(ConnectivityManager::parseLocalIPv4Address("192.0.2.1") ==
               0xC0000201UL,
           "IPv4 conversion puts the first octet in the high byte");
    expect(ConnectivityManager::parseLocalIPv4Address("192.0.2.256") == 0,
           "invalid IPv4 octet does not produce a partial address");
    expect(ConnectivityManager::parseLocalIPv4Address("192.0.2") == 0,
           "incomplete IPv4 text does not produce a partial address");
    expect(ConnectivityManager::parseLocalIPv4Address("192.0.2.1x") == 0,
           "invalid IPv4 text does not produce a partial address");
}

void testSuccessfulStartup() {
       FakeConnectivityOperations operations;
       operations.fakeConnectResult = true;
       operations.fakeTimeSynchronized = true;
       ConnectivityManager manager = createManager(operations);

    expect(manager.localIPv4Address() == 0,
           "manager starts without a local IPv4 address");
       expect(operations.addressReadCallCount == 0,
           "construction does not read the network address");
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
    expect(events.localAddressChanged,
           "successful startup emits an address change event");
    expect(manager.localIPv4Address() == 0xC0000201UL,
           "successful startup caches the local IPv4 address");
    expect(operations.addressReadCallCount == 1,
           "successful startup reads the address once");
    expect(operations.connectCallCount == 1,
           "startup performs one Wi-Fi connection attempt");
    expect(operations.disconnectCallCount == 1,
           "startup clears the previous Wi-Fi state once");

    operations.fakeNow = 999;
    expectNoEvents(manager.update(),
                   "connected status is not checked before one second");
    expect(operations.connectCallCount == 1,
           "connected update performs no extra connection attempt");
    expect(operations.addressReadCallCount == 1,
           "connected update does not read the address before one second");

    operations.fakeNow = 1000;
    expectNoEvents(manager.update(),
                   "unchanged address emits no repeated events");
    expect(operations.addressReadCallCount == 2,
           "connected status interval reads the address once");
}

void testConnectionFailureBackoff() {
    FakeConnectivityOperations operations;
    ConnectivityManager manager = createManager(operations);

    expectNoEvents(manager.update(),
                   "failed startup emits no connection events");
    expect(manager.localIPv4Address() == 0,
           "failed startup has no local IPv4 address");
    expect(operations.addressReadCallCount == 0,
           "failed startup does not read a stale platform address");
    expect(operations.connectCallCount == 1,
           "failed startup performs one connection attempt");

    operations.fakeNow = 4999;
    manager.update();
    expect(operations.connectCallCount == 1,
           "first retry does not occur before five seconds");

    operations.fakeNow = 5000;
    manager.update();
    expect(operations.connectCallCount == 2,
           "first retry occurs at five seconds");

    operations.fakeNow = 14999;
    manager.update();
    expect(operations.connectCallCount == 2,
           "second retry does not occur before ten more seconds");

    operations.fakeNow = 15000;
    operations.fakeConnectResult = true;
    ConnectivityEvents events = manager.update();
    expect(operations.connectCallCount == 3,
           "second retry occurs after ten more seconds");
    expect(events.wifiConnected,
           "successful retry emits connected event");
}

void testRetryStartsAfterBlockingAttempt() {
    FakeConnectivityOperations operations;
    operations.fakeNow = 100;
    operations.fakeConnectDuration = 2000;
    ConnectivityManager manager = createManager(operations);

    manager.update();
    expect(operations.fakeNow == 2100,
           "fake connection attempt advances the platform clock");

    operations.fakeConnectDuration = 0;
    operations.fakeNow = 7099;
    manager.update();
    expect(operations.connectCallCount == 1,
           "retry delay is measured from connection attempt completion");

    operations.fakeNow = 7100;
    manager.update();
    expect(operations.connectCallCount == 2,
           "retry occurs five seconds after attempt completion");
}

void testDisconnectAndReconnectEvents() {
    FakeConnectivityOperations operations;
    operations.fakeConnectResult = true;
    operations.fakeTimeSynchronized = true;
    ConnectivityManager manager = createManager(operations);
    manager.update();

    operations.fakeWiFiConnected = false;
    operations.fakeNow = 999;
    expectNoEvents(manager.update(),
                   "connection loss waits for the status interval");

       operations.fakeNow = 1000;
    ConnectivityEvents lost = manager.update();
    expect(lost.wifiDisconnected,
           "connection loss emits disconnected event");
    expect(!lost.wifiConnected,
           "loss update does not reconnect before application cleanup");
    expect(!manager.isWiFiConnected(),
           "manager clears Wi-Fi state after connection loss");
    expect(!manager.isTimeSynchronized(),
           "manager clears time state after connection loss");
    expect(lost.localAddressChanged,
           "connection loss emits an address change event");
    expect(manager.localIPv4Address() == 0,
           "connection loss clears the cached IPv4 address");
    expect(operations.addressReadCallCount == 1,
           "connection loss does not read a stale platform address");
    expect(operations.connectCallCount == 1,
           "connection loss does not reconnect in the same update");

    operations.fakeNow = 1001;
    ConnectivityEvents reconnected = manager.update();
    expect(reconnected.wifiConnected,
           "next update reconnects immediately after loss");
    expect(reconnected.localAddressChanged,
           "same-address reconnection emits a fresh address event");
    expect(manager.localIPv4Address() == 0xC0000201UL,
           "same-address reconnection restores the cached IPv4 address");
       expect(operations.connectCallCount == 2,
           "reconnection performs one new connection attempt");
}

void testLocalAddressChangesWhileConnected() {
       FakeConnectivityOperations operations;
       operations.fakeConnectResult = true;
       operations.fakeTimeSynchronized = true;
       ConnectivityManager manager = createManager(operations);
    manager.update();

       operations.fakeLocalIPv4Address = 0xC0000202UL;
       operations.fakeNow = 999;
    expectNoEvents(manager.update(),
                   "address changes wait for the status interval");
    expect(manager.localIPv4Address() == 0xC0000201UL,
           "cached address is stable before the status interval");

       operations.fakeNow = 1000;
    ConnectivityEvents changed = manager.update();
    expect(changed.localAddressChanged,
           "connected address change emits an event");
    expect(!changed.wifiConnected && !changed.wifiDisconnected &&
               !changed.timeSynchronized,
           "address changes do not emit Wi-Fi or time events");
    expect(manager.localIPv4Address() == 0xC0000202UL,
           "connected address change replaces the cached address");
    expect(manager.isWiFiConnected() && manager.isTimeSynchronized(),
           "address changes preserve Wi-Fi and time readiness");
    expect(operations.connectCallCount == 1 && operations.disconnectCallCount == 1,
           "address changes do not reconnect Wi-Fi");
    expect(operations.synchronizeCallCount == 0,
           "address changes do not trigger NTP synchronization");
    expect(operations.addressReadCallCount == 2,
           "address changes use the existing status sampling interval");

    operations.fakeNow = 1001;
    expectNoEvents(manager.update(),
                   "address change events last one update");
    expect(operations.addressReadCallCount == 2,
           "cached address access does not resample the network");
}

void testDelayedLocalAddress() {
    FakeConnectivityOperations operations;
    operations.fakeConnectResult = true;
    operations.fakeLocalIPv4Address = 0;
    ConnectivityManager manager = createManager(operations);

    ConnectivityEvents connected = manager.update();
    expect(connected.wifiConnected && !connected.localAddressChanged,
           "Wi-Fi can connect before acquiring an IPv4 address");
    expect(manager.localIPv4Address() == 0,
           "pending address acquisition leaves the cache empty");

       operations.fakeLocalIPv4Address = 0xC0000201UL;
       operations.fakeNow = 1000;
    ConnectivityEvents acquired = manager.update();
    expect(acquired.localAddressChanged && !acquired.wifiConnected,
           "delayed address acquisition emits only an address event");
    expect(manager.localIPv4Address() == 0xC0000201UL,
           "delayed address acquisition updates the cache");
    expect(!manager.isTimeSynchronized(),
           "address tracking does not require synchronized time");
}

void testLocalAddressLossAndRecovery() {
       FakeConnectivityOperations operations;
       operations.fakeConnectResult = true;
       ConnectivityManager manager = createManager(operations);
    manager.update();

       operations.fakeLocalIPv4Address = 0;
       operations.fakeNow = 1000;
    ConnectivityEvents lost = manager.update();
    expect(lost.localAddressChanged && manager.localIPv4Address() == 0,
           "address loss clears the cache and emits an event");
    expect(manager.isWiFiConnected() && !lost.wifiDisconnected,
           "address loss does not imply Wi-Fi disconnection");

       operations.fakeNow = 2000;
    expectNoEvents(manager.update(),
                   "continued address unavailability emits no events");

       operations.fakeLocalIPv4Address = 0xC0000202UL;
       operations.fakeNow = 3000;
    ConnectivityEvents recovered = manager.update();
    expect(recovered.localAddressChanged && !recovered.wifiConnected,
           "address recovery emits an event without reconnecting");
    expect(manager.localIPv4Address() == 0xC0000202UL,
           "address recovery caches the new IPv4 address");
    expect(operations.connectCallCount == 1,
           "address loss and recovery do not restart Wi-Fi");
}

void testNewAddressAfterFailedReconnect() {
    FakeConnectivityOperations operations;
    operations.fakeConnectResult = true;
    ConnectivityManager manager = createManager(operations);
    manager.update();

    operations.fakeWiFiConnected = false;
    operations.fakeNow = 1000;
    manager.update();
    operations.fakeConnectResult = false;
    operations.fakeNow = 1001;
    expectNoEvents(manager.update(),
                   "failed reconnect does not restore the old address");
    expect(manager.localIPv4Address() == 0 && operations.addressReadCallCount == 1,
           "failed reconnect leaves the cache empty without address reads");

    operations.fakeConnectResult = true;
    operations.fakeLocalIPv4Address = 0xC0000202UL;
    operations.fakeNow = 6000;
    expectNoEvents(manager.update(),
                   "new address does not bypass Wi-Fi retry backoff");
    expect(manager.localIPv4Address() == 0 && operations.addressReadCallCount == 1,
           "retry backoff does not expose the platform address");

    operations.fakeNow = 6001;
    ConnectivityEvents reconnected = manager.update();
    expect(reconnected.wifiConnected && reconnected.localAddressChanged,
           "successful reconnect emits connection and address events");
    expect(manager.localIPv4Address() == 0xC0000202UL,
           "successful reconnect reads the new IPv4 address");
       expect(operations.addressReadCallCount == 2,
           "successful reconnect samples the address once");
}

void testLocalAddressMillisWraparound() {
       FakeConnectivityOperations operations;
       operations.fakeNow = 0xFFFFFF00UL;
       operations.fakeConnectResult = true;
       ConnectivityManager manager = createManager(operations);
    manager.update();

       operations.fakeLocalIPv4Address = 0xC0000202UL;
       operations.fakeNow = 0xFFFFFF00UL + 999UL;
    expectNoEvents(manager.update(),
                   "wrapped address interval is not due early");
    expect(manager.localIPv4Address() == 0xC0000201UL &&
               operations.addressReadCallCount == 1,
           "wrapped address interval preserves the cached address");

       operations.fakeNow = 0xFFFFFF00UL + 1000UL;
    ConnectivityEvents changed = manager.update();
    expect(changed.localAddressChanged,
           "wrapped address interval detects changes on time");
    expect(manager.localIPv4Address() == 0xC0000202UL &&
               operations.addressReadCallCount == 2,
           "wrapped address interval reads and caches the new address");
}

void testNtpRetry() {
       FakeConnectivityOperations operations;
       operations.fakeConnectResult = true;
       ConnectivityManager manager = createManager(operations);
    manager.update();

    expect(!manager.isTimeSynchronized(),
           "manager records unsynchronized startup time");
    expect(operations.synchronizeCallCount == 0,
           "NTP is not retried immediately after connection");

    operations.fakeNow = 59999;
    manager.update();
    expect(operations.synchronizeCallCount == 0,
           "NTP is not retried before sixty seconds");

    operations.fakeNow = 60000;
    ConnectivityEvents failed = manager.update();
    expect(operations.synchronizeCallCount == 1,
           "NTP is retried at sixty seconds");
    expect(!failed.timeSynchronized,
           "failed NTP retry emits no synchronized event");

       operations.fakeNow = 119999;
    manager.update();
       expect(operations.synchronizeCallCount == 1,
           "second NTP retry waits another sixty seconds");

       operations.fakeNow = 120000;
       operations.fakeSyncResult = true;
    ConnectivityEvents synchronized = manager.update();
       expect(operations.synchronizeCallCount == 2,
           "second NTP retry occurs after sixty more seconds");
    expect(synchronized.timeSynchronized,
           "successful NTP retry emits synchronized event");
    expect(manager.isTimeSynchronized(),
           "manager records successful NTP synchronization");
}

void testStatusMillisWraparound() {
       FakeConnectivityOperations operations;
       operations.fakeNow = 0xFFFFFF00UL;
       operations.fakeConnectResult = true;
       operations.fakeTimeSynchronized = true;
       ConnectivityManager manager = createManager(operations);
    manager.update();

       operations.fakeWiFiConnected = false;
       operations.fakeNow = 0xFFFFFF00UL + 999UL;
    expectNoEvents(manager.update(),
                   "wrapped status interval waits until one second");

       operations.fakeNow = 0xFFFFFF00UL + 1000UL;
    ConnectivityEvents events = manager.update();
    expect(events.wifiDisconnected,
           "wrapped status interval detects loss on time");
}

void testRetryMillisWraparound() {
    FakeConnectivityOperations operations;
    operations.fakeNow = 0xFFFFFF00UL;
    ConnectivityManager manager = createManager(operations);
    manager.update();

    operations.fakeNow = 0xFFFFFF00UL + 4999UL;
    manager.update();
    expect(operations.connectCallCount == 1,
           "wrapped retry interval is not due one millisecond early");

    operations.fakeNow = 0xFFFFFF00UL + 5000UL;
    manager.update();
    expect(operations.connectCallCount == 2,
           "wrapped retry interval is due on time");
}

void testIndependentOperations() {
    FakeConnectivityOperations firstOperations;
    FakeConnectivityOperations secondOperations;
    firstOperations.fakeNow = 100;
    firstOperations.fakeConnectResult = true;
    secondOperations.fakeNow = 200;
    secondOperations.fakeLocalIPv4Address = 0xC0000202UL;
    ConnectivityManager firstManager = createManager(firstOperations);
    ConnectivityManager secondManager = createManager(secondOperations);

    expect(firstManager.update().wifiConnected,
           "first manager connects through its own operations instance");
    expectNoEvents(secondManager.update(),
                   "second manager uses its independent failed connection result");
    expect(firstManager.localIPv4Address() == 0xC0000201UL &&
               secondManager.localIPv4Address() == 0,
           "independent operations keep manager addresses separate");
    expect(firstOperations.connectCallCount == 1 &&
               secondOperations.connectCallCount == 1,
           "operation call counters belong to each fake instance");

    firstOperations.fakeNow = 1100;
    firstOperations.fakeWiFiConnected = false;
    expect(firstManager.update().wifiDisconnected,
           "manager observes later changes to its referenced operations");
    expectNoEvents(secondManager.update(),
                   "advancing the first clock does not advance the second retry");
    expect(secondOperations.fakeNow == 200 && secondOperations.connectCallCount == 1,
           "fake clocks and retry attempts do not share global state");

    secondOperations.fakeNow = 5200;
    secondOperations.fakeConnectResult = true;
    secondOperations.fakeTimeSynchronized = true;
    ConnectivityEvents connected = secondManager.update();
    expect(connected.wifiConnected && connected.timeSynchronized &&
               secondManager.localIPv4Address() == 0xC0000202UL,
           "second manager reconnects with its own address and synchronized time");
    expect(!firstManager.isWiFiConnected() && !firstManager.isTimeSynchronized() &&
               firstManager.localIPv4Address() == 0 &&
               firstOperations.connectCallCount == 1 &&
               secondOperations.connectCallCount == 2,
           "second manager updates do not change the first manager or backend");
}

void setup() {
    Serial.begin(115200);
    while (!Serial);
    delay(3000);

    Serial.println("TEST_SUITE: ConnectivityManagerTests");
       testIndependentOperations();
    testRetryDelayPolicy();
       testLocalIPv4AddressParsing();
    testSuccessfulStartup();
    testConnectionFailureBackoff();
    testRetryStartsAfterBlockingAttempt();
    testDisconnectAndReconnectEvents();
       testLocalAddressChangesWhileConnected();
       testDelayedLocalAddress();
       testLocalAddressLossAndRecovery();
       testNewAddressAfterFailedReconnect();
       testLocalAddressMillisWraparound();
    testNtpRetry();
    testStatusMillisWraparound();
    testRetryMillisWraparound();
}

void loop() {
    Serial.print("TEST_RESULT: ");
    Serial.println(failureCount == 0 ? "PASS" : "FAIL");
    delay(1000);
}