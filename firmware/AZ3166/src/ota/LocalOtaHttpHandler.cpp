#include "LocalOtaHttpHandler.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

const char OTA_PAGE[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport "
    "content=\"width=device-width\"><title>AZ3166 OTA</title><style>"
    "body{font:16px system-ui;max-width:42rem;margin:2rem auto;padding:0 1rem}"
    "input,button{font:inherit;margin:.3rem;padding:.5rem}pre{white-space:pre-wrap}"
    "</style><h1>AZ3166 Local OTA</h1><p>Hold both device buttons, then enter "
    "the displayed challenge.</p><label>Challenge <input id=c maxlength=8 "
    "placeholder=challenge></label><button onclick=claim()>Claim</button><br>"
    "<label>Signed package <input id=f type=file></label>"
    "<button onclick=upload()>Upload</button><button onclick=apply()>Apply</button>"
    "<pre id=o>Idle</pre><script>let k,g,d,v;const q=(p,x={})=>fetch(p,x).then("
    "async r=>{let j=await r.json();if(!r.ok)throw Error(j.error||r.status);return j"
    "}),h=()=>({Authorization:'OTA '+k});async function claim(){try{let j=await q("
    "'/api/ota/session',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({challenge:c.value})});k=j.capability;o.textContent='Armed'"
    "}catch(e){o.textContent=e}}async function upload(){try{let j=await q("
    "'/api/ota',{method:'POST',headers:{...h(),'Content-Type':"
    "'application/octet-stream'},body:f.files[0]});g=j.generation;d=j.digest;"
    "o.textContent=JSON.stringify(j,null,2)}catch(e){o.textContent=e}}async function "
    "apply(){try{let s=await q('/api/ota/status',{headers:h()});if(s.generation!==g"
    "||s.digest!==d)throw Error('staged image changed');await q('/api/ota/apply',"
    "{method:'POST',headers:{...h(),'Content-Type':'application/json'},body:JSON."
    "stringify({generation:g,digest:d})});o.textContent='Rebooting';setTimeout("
    "check,1500)}catch(e){o.textContent=e}}async function check(){try{let j=await "
    "q('/api/version');o.textContent='Firmware '+j.firmwareVersion}catch(e){"
    "setTimeout(check,1500)}}</script>";

static_assert(
    sizeof(OTA_PAGE) <= 3072,
    "OTA page exceeds the bounded HTTP response buffer");

}

LocalOtaHttpHandler::LocalOtaHttpHandler(
    LocalHttpHandler &fallback,
    LocalOtaController &controller)
    : fallback_(fallback), controller_(controller) {
}

bool LocalOtaHttpHandler::exactRoute(
    const char *requestLine, const char *method, const char *path) {
    if (requestLine == NULL || method == NULL || path == NULL) {
        return false;
    }
    char expected[96];
    int length = snprintf(expected, sizeof(expected), "%s %s HTTP/1.1",
                          method, path);
    return length > 0 && static_cast<size_t>(length) < sizeof(expected) &&
        strcmp(requestLine, expected) == 0;
}

bool LocalOtaHttpHandler::safeOrigin(const LocalHttpRequest &request) {
    if (request.hostCount > 1 || request.originCount > 1) {
        return false;
    }
    if (request.originCount == 0) {
        return true;
    }
    if (request.hostCount != 1 || request.origin == NULL ||
        request.host == NULL || strncmp(request.origin, "http://", 7) != 0) {
        return false;
    }
    return strcmp(request.origin + 7, request.host) == 0;
}

bool LocalOtaHttpHandler::validBodylessRequest(
    const LocalHttpRequest &request) {
    return request.bodyFramingValid &&
        !request.hasTransferEncoding &&
        !request.hasContentLength &&
        request.contentLength == 0 &&
        request.prefetchedLength == 0;
}

LocalHttpResponse LocalOtaHttpHandler::json(
    const char *status,
    const char *format,
    char *body,
    size_t bodySize,
    ...) {
    if (body == NULL || bodySize == 0) {
        return {
            "500 Internal Server Error", "application/json", 0,
            NULL, NULL, false
        };
    }
    va_list arguments;
    va_start(arguments, bodySize);
    int length = vsnprintf(body, bodySize, format, arguments);
    va_end(arguments);
    if (length < 0 || static_cast<size_t>(length) >= bodySize) {
        body[0] = '\0';
        return {
            "500 Internal Server Error", "application/json", 0,
            NULL, NULL, false
        };
    }
    return {
        status, "application/json", static_cast<size_t>(length),
        NULL, NULL, false
    };
}

LocalHttpResponse LocalOtaHttpHandler::otaPage(
    char *body, size_t bodySize) {
    size_t length = sizeof(OTA_PAGE) - 1;
    if (body == NULL || bodySize <= length) {
        return json(
            "500 Internal Server Error", "{\"error\":\"page unavailable\"}",
            body, bodySize);
    }
    memcpy(body, OTA_PAGE, length + 1);
    return {"200 OK", "text/html; charset=utf-8", length, NULL, NULL, false};
}

LocalHttpResponse LocalOtaHttpHandler::handle(
    const char *requestLine,
    char *body,
    size_t bodySize) {
    return fallback_.handle(requestLine, body, bodySize);
}

bool LocalOtaHttpHandler::requiresRequestMetadata(const char *requestLine) {
    return exactRoute(requestLine, "GET", "/api/ota/status") ||
        exactRoute(requestLine, "DELETE", "/api/ota");
}

LocalHttpResponse LocalOtaHttpHandler::handleRequest(
    const LocalHttpRequest &request,
    char *body,
    size_t bodySize) {
    if (request.requestLine == NULL || strstr(request.requestLine, "?") != NULL) {
        return json("404 Not Found", "{\"error\":\"not found\"}",
                    body, bodySize);
    }
    if (exactRoute(request.requestLine, "GET", "/ota")) {
        return otaPage(body, bodySize);
    }
    bool otaRoute =
        exactRoute(request.requestLine, "GET", "/api/ota/status") ||
        exactRoute(request.requestLine, "DELETE", "/api/ota");
    if (!otaRoute) {
        return fallback_.handleRequest(request, body, bodySize);
    }
    if (!safeOrigin(request) || request.hasCookie ||
        !validBodylessRequest(request)) {
        return json("400 Bad Request", "{\"error\":\"invalid request\"}",
                    body, bodySize);
    }
    uint32_t generation;
    if (!controller_.authorize(request, generation)) {
        return json("401 Unauthorized", "{\"error\":\"unauthorized\"}",
                    body, bodySize);
    }
    if (exactRoute(request.requestLine, "GET", "/api/ota/status")) {
        LocalOtaSnapshot status = controller_.snapshot();
        uint32_t stagedGeneration = 0;
        uint8_t digestBytes[OTA_SHA256_SIZE];
        char digest[(OTA_SHA256_SIZE * 2) + 1] = {};
        if (controller_.readyImage(stagedGeneration, digestBytes)) {
            encodeDigest(digestBytes, digest);
        }
        return json(
            "200 OK",
            "{\"state\":\"%s\",\"generation\":%lu,\"acceptedBytes\":%lu,"
            "\"totalBytes\":%lu,\"lastError\":\"%s\",\"digest\":\"%s\"}",
            body, bodySize,
            controller_.stateName(status.state),
            static_cast<unsigned long>(stagedGeneration),
            static_cast<unsigned long>(status.acceptedBytes),
            static_cast<unsigned long>(status.totalBytes),
            controller_.errorName(status.lastError),
            digest);
    }
    if (exactRoute(request.requestLine, "DELETE", "/api/ota")) {
        if (!controller_.cancel(request)) {
            return json("409 Conflict", "{\"error\":\"cannot cancel\"}",
                        body, bodySize);
        }
        return json("202 Accepted", "{\"status\":\"cancelling\"}",
                    body, bodySize);
    }

    return json("404 Not Found", "{\"error\":\"not found\"}", body, bodySize);
}

bool LocalOtaHttpHandler::handles(const char *requestLine) {
    return exactRoute(requestLine, "POST", "/api/ota/session") ||
        exactRoute(requestLine, "POST", "/api/ota") ||
        exactRoute(requestLine, "POST", "/api/ota/apply");
}

bool LocalOtaHttpHandler::readSmallBody(
    LocalHttpBodyStream &stream,
    char *body,
    size_t capacity,
    size_t &length) {
    length = 0;
    if (body == NULL || capacity < 2 || stream.remaining() >= capacity) {
        return false;
    }
    while (stream.remaining() != 0) {
        size_t received = 0;
        LocalHttpBodyReadStatus result =
            stream.read(body + length, capacity - 1 - length, received);
        if (result != LOCAL_HTTP_BODY_DATA || received == 0) {
            return false;
        }
        length += received;
    }
    body[length] = '\0';
    return true;
}

bool LocalOtaHttpHandler::parseChallenge(
    const char *body, char challenge[9]) {
    static const char PREFIX[] = "{\"challenge\":\"";
    if (body == NULL || strncmp(body, PREFIX, sizeof(PREFIX) - 1) != 0 ||
        strlen(body) != sizeof(PREFIX) - 1 + 8 + 2 ||
        strcmp(body + sizeof(PREFIX) - 1 + 8, "\"}") != 0) {
        return false;
    }

    memcpy(challenge, body + sizeof(PREFIX) - 1, 8);
    challenge[8] = '\0';
    return true;
}

bool LocalOtaHttpHandler::parseApply(
    const char *body,
    uint32_t &generation,
    uint8_t digest[OTA_SHA256_SIZE]) {
    static const char PREFIX[] = "{\"generation\":";
    static const char DIGEST[] = ",\"digest\":\"";
    if (body == NULL || digest == NULL ||
        strncmp(body, PREFIX, sizeof(PREFIX) - 1) != 0) {
        return false;
    }
    const char *cursor = body + sizeof(PREFIX) - 1;
    if (*cursor < '1' || *cursor > '9') {
        return false;
    }
    uint32_t value = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        uint32_t digit = static_cast<uint32_t>(*cursor - '0');
        if (value > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
        ++cursor;
    }
    if (strncmp(cursor, DIGEST, sizeof(DIGEST) - 1) != 0) {
        return false;
    }
    cursor += sizeof(DIGEST) - 1;
    for (size_t index = 0; index < OTA_SHA256_SIZE; ++index) {
        uint8_t byte = 0;
        for (size_t nibble = 0; nibble < 2; ++nibble) {
            char character = *cursor++;
            if (character >= '0' && character <= '9') {
                byte = static_cast<uint8_t>((byte << 4) | (character - '0'));
            } else if (character >= 'a' && character <= 'f') {
                byte = static_cast<uint8_t>(
                    (byte << 4) | (character - 'a' + 10));
            } else {
                return false;
            }
        }
        digest[index] = byte;
    }
    if (strcmp(cursor, "\"}") != 0) {
        return false;
    }
    generation = value;
    return true;
}

void LocalOtaHttpHandler::encodeDigest(
    const uint8_t digest[OTA_SHA256_SIZE],
    char output[(OTA_SHA256_SIZE * 2) + 1]) {
    static const char HEX[] = "0123456789abcdef";
    for (size_t index = 0; index < OTA_SHA256_SIZE; ++index) {
        output[index * 2] = HEX[digest[index] >> 4];
        output[index * 2 + 1] = HEX[digest[index] & 0x0f];
    }
    output[OTA_SHA256_SIZE * 2] = '\0';
}

LocalHttpResponse LocalOtaHttpHandler::handle(
    const LocalHttpStreamingRequest &request,
    LocalHttpBodyStream &body,
    char *responseBody,
    size_t responseBodySize) {
    if (!request.metadata.bodyFramingValid ||
        !safeOrigin(request.metadata) || request.metadata.hasCookie ||
        strstr(request.requestLine, "?") != NULL) {
        LocalHttpResponse response = json(
            "400 Bad Request", "{\"error\":\"invalid request\"}",
            responseBody, responseBodySize);
        response.allowUnreadRequestBody = true;
        return response;
    }
    if (exactRoute(request.requestLine, "POST", "/api/ota/session")) {
        if (request.metadata.authorizationCount != 0 ||
            request.contentLength > 64 ||
            request.metadata.contentTypeCount != 1 ||
            strcmp(request.metadata.contentType, "application/json") != 0) {
            LocalHttpResponse response = json(
                "400 Bad Request", "{\"error\":\"invalid claim\"}",
                responseBody, responseBodySize);
            response.allowUnreadRequestBody = true;
            return response;
        }
        char requestBody[65];
        size_t length;
        char challenge[9];
        char capability[33];
        if (!readSmallBody(body, requestBody, sizeof(requestBody), length) ||
            !parseChallenge(requestBody, challenge) ||
            !controller_.claim(
                challenge, request.metadata.peerAddress,
                request.metadata.networkGeneration, capability)) {
            return json("401 Unauthorized", "{\"error\":\"unauthorized\"}",
                        responseBody, responseBodySize);
        }
        return json("201 Created", "{\"capability\":\"%s\"}",
                    responseBody, responseBodySize, capability);
    }

    uint32_t generation;
    if (!controller_.authorize(request.metadata, generation)) {
        LocalHttpResponse response = json(
            "401 Unauthorized", "{\"error\":\"unauthorized\"}",
            responseBody, responseBodySize);
        response.allowUnreadRequestBody = true;
        return response;
    }
    if (exactRoute(request.requestLine, "POST", "/api/ota/apply")) {
        if (request.contentLength > 128 ||
            request.metadata.contentTypeCount != 1 ||
            strcmp(request.metadata.contentType, "application/json") != 0) {
            LocalHttpResponse response = json(
                "400 Bad Request", "{\"error\":\"invalid apply\"}",
                responseBody, responseBodySize);
            response.allowUnreadRequestBody = true;
            return response;
        }
        char requestBody[129];
        size_t length = 0;
        uint32_t expectedGeneration = 0;
        uint8_t expectedDigest[OTA_SHA256_SIZE];
        uint32_t stagedGeneration = 0;
        uint8_t stagedDigest[OTA_SHA256_SIZE];
        if (!readSmallBody(
                body, requestBody, sizeof(requestBody), length) ||
            !parseApply(
                requestBody, expectedGeneration, expectedDigest) ||
            !controller_.readyImage(stagedGeneration, stagedDigest) ||
            expectedGeneration != stagedGeneration ||
            memcmp(expectedDigest, stagedDigest, OTA_SHA256_SIZE) != 0) {
            return json("409 Conflict", "{\"error\":\"staged image mismatch\"}",
                        responseBody, responseBodySize);
        }
        OTAStagingError result;
        if (!controller_.apply(request.metadata, result)) {
            return json("409 Conflict", "{\"error\":\"not ready\"}",
                        responseBody, responseBodySize);
        }
        if (result == OTA_OK) {
            LocalHttpResponse response = json(
                "202 Accepted", "{\"status\":\"reboot scheduled\"}",
                responseBody, responseBodySize);
            response.afterAttempt = afterApplyResponse;
            response.afterAttemptContext = &controller_;
            return response;
        }
        if (result == OTA_ERROR_ACTIVATION_UNCERTAIN) {
            return json(
                "503 Service Unavailable",
                "{\"error\":\"activation uncertain; restore with ST-Link\"}",
                responseBody, responseBodySize);
        }
        if (result == OTA_ERROR_CANCELLED) {
            return json("409 Conflict", "{\"error\":\"apply cancelled\"}",
                        responseBody, responseBodySize);
        }
        return json("500 Internal Server Error",
                    "{\"error\":\"activation failed\"}",
                    responseBody, responseBodySize);
    }
    if (request.metadata.contentTypeCount != 1 ||
        strcmp(request.metadata.contentType, "application/octet-stream") != 0 ||
        request.contentLength < OTA_PACKAGE_PAYLOAD_OFFSET + 1) {
        LocalHttpResponse response = json(
            "400 Bad Request", "{\"error\":\"invalid upload\"}",
            responseBody, responseBodySize);
        response.allowUnreadRequestBody = true;
        return response;
    }
    if (!controller_.upload(request, body)) {
        LocalOtaSnapshot status = controller_.snapshot();
        const char *httpStatus =
            status.state == LOCAL_OTA_ERROR ? "422 Unprocessable Entity" :
            "409 Conflict";
        return json(httpStatus, "{\"error\":\"%s\"}",
                    responseBody, responseBodySize,
                    controller_.errorName(status.lastError));
    }
    LocalOtaSnapshot status = controller_.snapshot();
    uint32_t stagedGeneration = 0;
    uint8_t digestBytes[OTA_SHA256_SIZE];
    char digest[(OTA_SHA256_SIZE * 2) + 1] = {};
    if (controller_.readyImage(stagedGeneration, digestBytes)) {
        encodeDigest(digestBytes, digest);
    }
    return json(
        "201 Created",
        "{\"state\":\"Ready\",\"generation\":%lu,\"acceptedBytes\":%lu,"
        "\"digest\":\"%s\"}",
        responseBody, responseBodySize,
        static_cast<unsigned long>(stagedGeneration),
        static_cast<unsigned long>(status.acceptedBytes),
        digest);
}

void LocalOtaHttpHandler::afterApplyResponse(bool sent, void *context) {
    static_cast<LocalOtaController *>(context)->responseAttempted(sent);
}
