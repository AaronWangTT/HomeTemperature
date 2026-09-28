#ifndef LOCAL_OTA_HTTP_HANDLER_H
#define LOCAL_OTA_HTTP_HANDLER_H

#include "../http/LocalHttpHandler.h"
#include "../http/LocalHttpStreamingHandler.h"
#include "LocalOtaController.h"

class LocalOtaHttpHandler :
    public LocalHttpHandler,
    public LocalHttpStreamingHandler {
public:
    LocalOtaHttpHandler(
        LocalHttpHandler &fallback,
        LocalOtaController &controller);

    LocalHttpResponse handle(
        const char *requestLine,
        char *body,
        size_t bodySize) override;
    LocalHttpResponse handleRequest(
        const LocalHttpRequest &request,
        char *body,
        size_t bodySize) override;
    bool requiresRequestMetadata(const char *requestLine) override;
    bool handles(const char *requestLine) override;
    LocalHttpResponse handle(
        const LocalHttpStreamingRequest &request,
        LocalHttpBodyStream &body,
        char *responseBody,
        size_t responseBodySize) override;

private:
    static bool exactRoute(
        const char *requestLine, const char *method, const char *path);
    static bool safeOrigin(const LocalHttpRequest &request);
    static bool validBodylessRequest(const LocalHttpRequest &request);
    static bool readSmallBody(
        LocalHttpBodyStream &stream, char *body, size_t capacity, size_t &length);
    static bool parseApply(
        const char *body,
        uint32_t &generation,
        uint8_t digest[OTA_SHA256_SIZE]);
    static void encodeDigest(
        const uint8_t digest[OTA_SHA256_SIZE],
        char output[(OTA_SHA256_SIZE * 2) + 1]);
    static LocalHttpResponse json(
        const char *status,
        const char *format,
        char *body,
        size_t bodySize,
        ...);
    static LocalHttpResponse otaPage(char *body, size_t bodySize);
    LocalHttpHandler &fallback_;
    LocalOtaController &controller_;
};

#endif
