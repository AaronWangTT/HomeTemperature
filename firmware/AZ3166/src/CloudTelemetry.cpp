#include "CloudTelemetry.h"

#include <Arduino.h>
#include "http_client.h"

namespace {

const char API_KEY_HEADER[] = "X-Device-Key";
const char CONTENT_TYPE[] = "application/json";
const char ACCEPT[] = "application/json";
const char CONNECTION[] = "close";

TelemetryUploadResult sendPlatformRequest(
    const CloudTelemetryRequest &cloudRequest,
    CloudTelemetryResponseHandler responseHandler) {
    HTTPClient request(
        cloudRequest.rootCertificate,
        HTTP_POST,
        cloudRequest.endpoint);
    request.set_header("Content-Type", cloudRequest.contentType);
    request.set_header("Accept", cloudRequest.accept);
    request.set_header(
        cloudRequest.apiKeyHeader,
        cloudRequest.apiKey);
    request.set_header("Connection", cloudRequest.connection);

    const Http_Response *response = request.send(
        cloudRequest.payload,
        static_cast<int>(cloudRequest.payloadLength));

    CloudTelemetryResponse cloudResponse;
    if (response == NULL) {
        cloudResponse.received = false;
        cloudResponse.statusCode = 0;
        cloudResponse.networkError = static_cast<int>(request.get_error());
        cloudResponse.body = NULL;
        cloudResponse.bodyLength = 0;
    } else {
        cloudResponse.received = true;
        cloudResponse.statusCode = response->status_code;
        cloudResponse.networkError = 0;
        cloudResponse.body = response->body;
        cloudResponse.bodyLength = response->body_length > 0
            ? static_cast<size_t>(response->body_length)
            : 0;
    }

    return responseHandler(cloudResponse);
}

CloudTelemetryOperations platformOperations = {
    sendPlatformRequest
};

}  // namespace

CloudTelemetry::CloudTelemetry(
    const char *endpoint,
    const char *rootCertificate,
    const char *apiKey,
    const char *apiKeyPlaceholder)
        : CloudTelemetry(
                    endpoint,
                    rootCertificate,
                    apiKey,
                    apiKeyPlaceholder,
                    platformOperations) {
}

CloudTelemetry::CloudTelemetry(
        const char *endpoint,
        const char *rootCertificate,
        const char *apiKey,
        const char *apiKeyPlaceholder,
        const CloudTelemetryOperations &operations)
    : endpoint_(endpoint),
      rootCertificate_(rootCertificate),
      apiKey_(apiKey),
            apiKeyPlaceholder_(apiKeyPlaceholder),
            operations_(operations) {
}

void CloudTelemetry::begin() const {
    if (isConfigured()) {
        Serial.print("Cloud telemetry endpoint: ");
        Serial.println(endpoint_);
    } else {
        Serial.println("Cloud upload disabled: set CLOUD_DEVICE_API_KEY");
    }
}

bool CloudTelemetry::isConfigured() const {
    return isApiKeyConfigured(apiKey_, apiKeyPlaceholder_);
}

TelemetryUploadResult CloudTelemetry::upload(
    const char *payload,
    size_t payloadLength) {
    if (!isConfigured()) {
        Serial.println("Cloud upload skipped: device API key is not configured");
        return {TELEMETRY_UPLOAD_DISABLED, 0};
    }

    if (payload == NULL || payloadLength == 0) {
        Serial.println("Cloud upload skipped: payload is empty");
        return {TELEMETRY_UPLOAD_PAYLOAD_INVALID, 0};
    }

    if (operations_.send == NULL) {
        Serial.println("Cloud upload skipped: HTTPS transport is unavailable");
        return {TELEMETRY_UPLOAD_DISABLED, 0};
    }

    Serial.print("Cloud telemetry JSON: ");
    Serial.write(
        reinterpret_cast<const uint8_t *>(payload),
        payloadLength);
    Serial.println();

    Serial.print("Uploading telemetry: ");
    CloudTelemetryRequest request = {
        endpoint_,
        rootCertificate_,
        API_KEY_HEADER,
        apiKey_,
        CONTENT_TYPE,
        ACCEPT,
        CONNECTION,
        payload,
        payloadLength
    };
    return operations_.send(request, handleResponse);
}

TelemetryUploadResult CloudTelemetry::handleResponse(
    const CloudTelemetryResponse &response) {
    if (!response.received) {
        Serial.print("failed, network error ");
        Serial.println(response.networkError);
        return {
            TELEMETRY_UPLOAD_NETWORK_ERROR,
            response.networkError
        };
    }

    Serial.print("HTTP ");
    Serial.println(response.statusCode);
    if (isSuccessfulStatus(response.statusCode)) {
        return {
            TELEMETRY_UPLOAD_SUCCESS,
            response.statusCode
        };
    }

    if (response.body != NULL && response.bodyLength > 0) {
        Serial.write(
            reinterpret_cast<const uint8_t *>(response.body),
            response.bodyLength);
        Serial.println();
    }

    if (isRetryableStatus(response.statusCode)) {
        return {
            TELEMETRY_UPLOAD_HTTP_RETRYABLE,
            response.statusCode
        };
    }

    return {
        TELEMETRY_UPLOAD_HTTP_REJECTED,
        response.statusCode
    };
}

const char *CloudTelemetry::endpoint() const {
    return endpoint_;
}

bool CloudTelemetry::isApiKeyConfigured(
    const char *apiKey,
    const char *apiKeyPlaceholder) {
    if (apiKey == NULL || strlen(apiKey) < 32) {
        return false;
    }

    return apiKeyPlaceholder == NULL ||
           strcmp(apiKey, apiKeyPlaceholder) != 0;
}

bool CloudTelemetry::isSuccessfulStatus(int statusCode) {
    return statusCode == 201;
}

bool CloudTelemetry::isRetryableStatus(int statusCode) {
    return statusCode == 408 ||
           statusCode == 425 ||
           statusCode == 429 ||
           (statusCode >= 500 && statusCode <= 599);
}