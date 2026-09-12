#include <Arduino.h>
#include <type_traits>

#include "src/cloud/CloudTelemetry.h"

const char VALID_API_KEY[] =
    "0123456789ABCDEF0123456789ABCDEF";
const char PLACEHOLDER_API_KEY[] =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

int failureCount = 0;

static_assert(std::is_abstract<CloudTelemetryOperations>::value,
              "CloudTelemetryOperations must require a send implementation");
static_assert(std::has_virtual_destructor<CloudTelemetryOperations>::value,
              "CloudTelemetryOperations must have a virtual destructor");

class FakeCloudTelemetryOperations : public CloudTelemetryOperations {
public:
    int sendCallCount = 0;
    CloudTelemetryRequest capturedRequest = {};
    CloudTelemetryResponse fakeResponse = {true, 201, 0, NULL, 0};

    TelemetryUploadResult send(
        const CloudTelemetryRequest &request,
        CloudTelemetryResponseHandler responseHandler) override {
        ++sendCallCount;
        capturedRequest = request;
        return responseHandler(fakeResponse);
    }
};

class UnavailableCloudTelemetryOperations : public CloudTelemetryOperations {
public:
    TelemetryUploadResult send(
        const CloudTelemetryRequest &,
        CloudTelemetryResponseHandler) override {
        return {TELEMETRY_UPLOAD_DISABLED, 0};
    }
};

CloudTelemetry createFakeCloudTelemetry(CloudTelemetryOperations &operations) {
    return CloudTelemetry(
        "https://example.test/api/telemetry",
        "test-certificate",
        VALID_API_KEY,
        PLACEHOLDER_API_KEY,
        operations);
}

void expect(bool condition, const char *name) {
    Serial.print(condition ? "PASS: " : "FAIL: ");
    Serial.println(name);
    if (!condition) {
        ++failureCount;
    }
}

void testApiKeyValidation() {
    expect(
        !CloudTelemetry::isApiKeyConfigured(NULL, PLACEHOLDER_API_KEY),
        "null API key is not configured");
    expect(
        !CloudTelemetry::isApiKeyConfigured("", PLACEHOLDER_API_KEY),
        "empty API key is not configured");
    expect(
        !CloudTelemetry::isApiKeyConfigured(
            "1234567890123456789012345678901",
            PLACEHOLDER_API_KEY),
        "31-character API key is not configured");
    expect(
        CloudTelemetry::isApiKeyConfigured(
            VALID_API_KEY,
            PLACEHOLDER_API_KEY),
        "32-character API key is configured");
    expect(
        !CloudTelemetry::isApiKeyConfigured(
            PLACEHOLDER_API_KEY,
            PLACEHOLDER_API_KEY),
        "placeholder API key is not configured");
    expect(
        CloudTelemetry::isApiKeyConfigured(VALID_API_KEY, NULL),
        "null placeholder does not reject a valid API key");
}

void testObjectConfiguration() {
    CloudTelemetry configured(
        "https://example.test/api/telemetry",
        "test-certificate",
        VALID_API_KEY,
        PLACEHOLDER_API_KEY);
    expect(configured.isConfigured(),
           "cloud telemetry reports a valid configuration");
    expect(
        strcmp(
            configured.endpoint(),
            "https://example.test/api/telemetry") == 0,
        "cloud telemetry exposes its configured endpoint");

    CloudTelemetry disabled(
        "https://example.test/api/telemetry",
        "test-certificate",
        "short-key",
        PLACEHOLDER_API_KEY);
    expect(!disabled.isConfigured(),
           "cloud telemetry reports an invalid configuration");
    TelemetryUploadResult result = disabled.upload("{}", 2);
    expect(result.status == TELEMETRY_UPLOAD_DISABLED,
           "invalid configuration is rejected before network upload");
}

void testPayloadValidation() {
    FakeCloudTelemetryOperations operations;
    CloudTelemetry telemetry = createFakeCloudTelemetry(operations);

    TelemetryUploadResult nullPayload = telemetry.upload(NULL, 2);
    expect(
        nullPayload.status == TELEMETRY_UPLOAD_PAYLOAD_INVALID,
        "null payload is rejected");
    TelemetryUploadResult emptyPayload = telemetry.upload("", 0);
    expect(
        emptyPayload.status == TELEMETRY_UPLOAD_PAYLOAD_INVALID,
        "empty payload is rejected");
        expect(operations.sendCallCount == 0,
           "invalid payload never reaches HTTPS transport");

        UnavailableCloudTelemetryOperations unavailableOperations;
    CloudTelemetry unavailable(
        "https://example.test/api/telemetry",
        "test-certificate",
        VALID_API_KEY,
        PLACEHOLDER_API_KEY,
        unavailableOperations);
    TelemetryUploadResult unavailableResult =
        unavailable.upload("{}", 2);
    expect(
        unavailableResult.status == TELEMETRY_UPLOAD_DISABLED,
        "an explicit unavailable HTTPS backend returns a disabled upload");
}

void testRequestContract() {
    FakeCloudTelemetryOperations operations;
    CloudTelemetry telemetry = createFakeCloudTelemetry(operations);
    const char payload[] = "{\"temperature\":23.5}";

    TelemetryUploadResult result = telemetry.upload(
        payload,
        sizeof(payload) - 1);
    expect(result.status == TELEMETRY_UPLOAD_SUCCESS,
           "HTTP 201 completes a cloud upload");
        expect(operations.sendCallCount == 1,
           "valid payload performs one HTTPS request");
        expect(strcmp(operations.capturedRequest.endpoint,
                  "https://example.test/api/telemetry") == 0,
           "request carries the configured endpoint");
    expect(strcmp(operations.capturedRequest.rootCertificate,
                  "test-certificate") == 0,
           "request carries the configured root certificate");
    expect(strcmp(operations.capturedRequest.apiKeyHeader,
                  "X-Device-Key") == 0,
           "request uses the device key header");
    expect(strcmp(operations.capturedRequest.apiKey, VALID_API_KEY) == 0,
           "request carries the configured API key");
    expect(strcmp(operations.capturedRequest.contentType,
                  "application/json") == 0,
           "request uses JSON content type");
    expect(strcmp(operations.capturedRequest.accept,
                  "application/json") == 0,
           "request accepts JSON responses");
    expect(strcmp(operations.capturedRequest.connection, "close") == 0,
           "request closes the HTTPS connection");
    expect(operations.capturedRequest.payloadLength == sizeof(payload) - 1,
           "request preserves payload length");
    expect(memcmp(operations.capturedRequest.payload,
                  payload,
                  sizeof(payload) - 1) == 0,
           "request preserves payload bytes");
}

void testResponseHandling() {
    FakeCloudTelemetryOperations operations;
    CloudTelemetry telemetry = createFakeCloudTelemetry(operations);

    operations.fakeResponse.received = false;
    operations.fakeResponse.networkError = -3001;
    TelemetryUploadResult networkError = telemetry.upload("{}", 2);
    expect(
        networkError.status == TELEMETRY_UPLOAD_NETWORK_ERROR,
        "network error is classified as retryable transport failure");
    expect(networkError.detailCode == -3001,
           "network error code is preserved");

    operations.fakeResponse.received = true;
    operations.fakeResponse.statusCode = 401;
    operations.fakeResponse.body = "{\"error\":\"unauthorized\"}";
    operations.fakeResponse.bodyLength = strlen(operations.fakeResponse.body);
    TelemetryUploadResult rejected = telemetry.upload("{}", 2);
    expect(
        rejected.status == TELEMETRY_UPLOAD_HTTP_REJECTED,
        "HTTP 401 is classified as a rejected upload");
    expect(rejected.detailCode == 401,
           "rejected HTTP status is preserved");

    operations.fakeResponse.statusCode = 422;
    operations.fakeResponse.body = "{\"detail\":\"invalid telemetry\"}";
    operations.fakeResponse.bodyLength = strlen(operations.fakeResponse.body);
    TelemetryUploadResult invalidReading = telemetry.upload("{}", 2);
    expect(
        invalidReading.status == TELEMETRY_UPLOAD_HTTP_RETRYABLE,
        "HTTP 422 is retryable after transient sensor validation failures");
    expect(invalidReading.detailCode == 422,
           "HTTP 422 status is preserved");

    operations.fakeResponse.statusCode = 429;
    TelemetryUploadResult throttled = telemetry.upload("{}", 2);
    expect(
        throttled.status == TELEMETRY_UPLOAD_HTTP_RETRYABLE,
        "HTTP 429 is classified as retryable");

    operations.fakeResponse.statusCode = 500;
    TelemetryUploadResult serverError = telemetry.upload("{}", 2);
    expect(
        serverError.status == TELEMETRY_UPLOAD_HTTP_RETRYABLE,
        "HTTP 500 is classified as retryable");

    operations.fakeResponse.statusCode = 201;
    operations.fakeResponse.body = NULL;
    operations.fakeResponse.bodyLength = 0;
    TelemetryUploadResult success = telemetry.upload("{}", 2);
    expect(success.status == TELEMETRY_UPLOAD_SUCCESS,
           "HTTP 201 succeeds after earlier failures");
}

void testIndependentOperations() {
    FakeCloudTelemetryOperations firstOperations;
    FakeCloudTelemetryOperations secondOperations;
    secondOperations.fakeResponse.statusCode = 401;
    CloudTelemetry first = createFakeCloudTelemetry(firstOperations);
    CloudTelemetry second = createFakeCloudTelemetry(secondOperations);
    const char firstPayload[] = "{}";
    const char secondPayload[] = "{\"sample\":2}";

    expect(first.upload(firstPayload, sizeof(firstPayload) - 1).status ==
               TELEMETRY_UPLOAD_SUCCESS,
           "first cloud object uses its own backend result");
    expect(second.upload(secondPayload, sizeof(secondPayload) - 1).status ==
               TELEMETRY_UPLOAD_HTTP_REJECTED,
           "second cloud object uses its independent backend result");
    expect(firstOperations.sendCallCount == 1 && secondOperations.sendCallCount == 1 &&
               firstOperations.capturedRequest.payload == firstPayload &&
               secondOperations.capturedRequest.payload == secondPayload,
           "cloud backends retain separate call counters and captured requests");

    firstOperations.fakeResponse.statusCode = 503;
    expect(first.upload(firstPayload, sizeof(firstPayload) - 1).status ==
               TELEMETRY_UPLOAD_HTTP_RETRYABLE &&
               secondOperations.fakeResponse.statusCode == 401 &&
               secondOperations.sendCallCount == 1,
           "mutating a referenced backend does not change another backend");
}

void testHttpStatusContract() {
    expect(CloudTelemetry::isSuccessfulStatus(201),
           "HTTP 201 is a successful telemetry response");
    expect(!CloudTelemetry::isSuccessfulStatus(200),
           "HTTP 200 is not the ingestion success contract");
    expect(!CloudTelemetry::isSuccessfulStatus(202),
           "HTTP 202 is not the ingestion success contract");
    expect(!CloudTelemetry::isSuccessfulStatus(400),
           "HTTP 400 is a failed telemetry response");
    expect(!CloudTelemetry::isSuccessfulStatus(401),
           "HTTP 401 is a failed telemetry response");
    expect(!CloudTelemetry::isSuccessfulStatus(500),
           "HTTP 500 is a failed telemetry response");
    expect(CloudTelemetry::isRetryableStatus(408),
           "HTTP 408 is retryable");
    expect(CloudTelemetry::isRetryableStatus(422),
           "HTTP 422 is retryable");
    expect(CloudTelemetry::isRetryableStatus(429),
           "HTTP 429 is retryable");
    expect(CloudTelemetry::isRetryableStatus(503),
           "HTTP 503 is retryable");
    expect(!CloudTelemetry::isRetryableStatus(401),
           "HTTP 401 is not retryable");
}

void setup() {
    Serial.begin(115200);
    while (!Serial);
    delay(3000);

    Serial.println("TEST_SUITE: CloudTelemetryTests");
    testApiKeyValidation();
    testObjectConfiguration();
    testPayloadValidation();
    testRequestContract();
    testResponseHandling();
    testIndependentOperations();
    testHttpStatusContract();
}

void loop() {
    Serial.print("TEST_RESULT: ");
    Serial.println(failureCount == 0 ? "PASS" : "FAIL");
    delay(1000);
}