#include <Arduino.h>

#include "CloudTelemetry.h"

const char VALID_API_KEY[] =
    "0123456789ABCDEF0123456789ABCDEF";
const char PLACEHOLDER_API_KEY[] =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

int failureCount = 0;
int sendCallCount = 0;
CloudTelemetryRequest capturedRequest;
CloudTelemetryResponse fakeResponse;

TelemetryUploadResult sendFakeRequest(
    const CloudTelemetryRequest &request,
    CloudTelemetryResponseHandler responseHandler) {
    ++sendCallCount;
    capturedRequest = request;
    return responseHandler(fakeResponse);
}

CloudTelemetry createFakeCloudTelemetry() {
    CloudTelemetryOperations operations = {
        sendFakeRequest
    };
    return CloudTelemetry(
        "https://example.test/api/telemetry",
        "test-certificate",
        VALID_API_KEY,
        PLACEHOLDER_API_KEY,
        operations);
}

void resetFakeTransport() {
    sendCallCount = 0;
    fakeResponse.received = true;
    fakeResponse.statusCode = 201;
    fakeResponse.networkError = 0;
    fakeResponse.body = NULL;
    fakeResponse.bodyLength = 0;
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
    resetFakeTransport();
    CloudTelemetry telemetry = createFakeCloudTelemetry();

    TelemetryUploadResult nullPayload = telemetry.upload(NULL, 2);
    expect(
        nullPayload.status == TELEMETRY_UPLOAD_PAYLOAD_INVALID,
        "null payload is rejected");
    TelemetryUploadResult emptyPayload = telemetry.upload("", 0);
    expect(
        emptyPayload.status == TELEMETRY_UPLOAD_PAYLOAD_INVALID,
        "empty payload is rejected");
    expect(sendCallCount == 0,
           "invalid payload never reaches HTTPS transport");

    CloudTelemetryOperations unavailableOperations = {NULL};
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
        "missing HTTPS transport is rejected");
}

void testRequestContract() {
    resetFakeTransport();
    CloudTelemetry telemetry = createFakeCloudTelemetry();
    const char payload[] = "{\"temperature\":23.5}";

    TelemetryUploadResult result = telemetry.upload(
        payload,
        sizeof(payload) - 1);
    expect(result.status == TELEMETRY_UPLOAD_SUCCESS,
           "HTTP 201 completes a cloud upload");
    expect(sendCallCount == 1,
           "valid payload performs one HTTPS request");
    expect(strcmp(capturedRequest.endpoint,
                  "https://example.test/api/telemetry") == 0,
           "request carries the configured endpoint");
    expect(strcmp(capturedRequest.rootCertificate,
                  "test-certificate") == 0,
           "request carries the configured root certificate");
    expect(strcmp(capturedRequest.apiKeyHeader,
                  "X-Device-Key") == 0,
           "request uses the device key header");
    expect(strcmp(capturedRequest.apiKey, VALID_API_KEY) == 0,
           "request carries the configured API key");
    expect(strcmp(capturedRequest.contentType,
                  "application/json") == 0,
           "request uses JSON content type");
    expect(strcmp(capturedRequest.accept,
                  "application/json") == 0,
           "request accepts JSON responses");
    expect(strcmp(capturedRequest.connection, "close") == 0,
           "request closes the HTTPS connection");
    expect(capturedRequest.payloadLength == sizeof(payload) - 1,
           "request preserves payload length");
    expect(memcmp(capturedRequest.payload,
                  payload,
                  sizeof(payload) - 1) == 0,
           "request preserves payload bytes");
}

void testResponseHandling() {
    resetFakeTransport();
    CloudTelemetry telemetry = createFakeCloudTelemetry();

    fakeResponse.received = false;
    fakeResponse.networkError = -3001;
    TelemetryUploadResult networkError = telemetry.upload("{}", 2);
    expect(
        networkError.status == TELEMETRY_UPLOAD_NETWORK_ERROR,
        "network error is classified as retryable transport failure");
    expect(networkError.detailCode == -3001,
           "network error code is preserved");

    fakeResponse.received = true;
    fakeResponse.statusCode = 401;
    fakeResponse.body = "{\"error\":\"unauthorized\"}";
    fakeResponse.bodyLength = strlen(fakeResponse.body);
    TelemetryUploadResult rejected = telemetry.upload("{}", 2);
    expect(
        rejected.status == TELEMETRY_UPLOAD_HTTP_REJECTED,
        "HTTP 401 is classified as a rejected upload");
    expect(rejected.detailCode == 401,
           "rejected HTTP status is preserved");

    fakeResponse.statusCode = 422;
    fakeResponse.body = "{\"detail\":\"invalid telemetry\"}";
    fakeResponse.bodyLength = strlen(fakeResponse.body);
    TelemetryUploadResult invalidReading = telemetry.upload("{}", 2);
    expect(
        invalidReading.status == TELEMETRY_UPLOAD_HTTP_RETRYABLE,
        "HTTP 422 is retryable after transient sensor validation failures");
    expect(invalidReading.detailCode == 422,
           "HTTP 422 status is preserved");

    fakeResponse.statusCode = 429;
    TelemetryUploadResult throttled = telemetry.upload("{}", 2);
    expect(
        throttled.status == TELEMETRY_UPLOAD_HTTP_RETRYABLE,
        "HTTP 429 is classified as retryable");

    fakeResponse.statusCode = 500;
    TelemetryUploadResult serverError = telemetry.upload("{}", 2);
    expect(
        serverError.status == TELEMETRY_UPLOAD_HTTP_RETRYABLE,
        "HTTP 500 is classified as retryable");

    fakeResponse.statusCode = 201;
    fakeResponse.body = NULL;
    fakeResponse.bodyLength = 0;
    TelemetryUploadResult success = telemetry.upload("{}", 2);
    expect(success.status == TELEMETRY_UPLOAD_SUCCESS,
           "HTTP 201 succeeds after earlier failures");
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
    testHttpStatusContract();
}

void loop() {
    Serial.print("TEST_RESULT: ");
    Serial.println(failureCount == 0 ? "PASS" : "FAIL");
    delay(1000);
}