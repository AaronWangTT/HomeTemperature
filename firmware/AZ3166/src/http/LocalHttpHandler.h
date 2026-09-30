#ifndef LOCAL_HTTP_HANDLER_H
#define LOCAL_HTTP_HANDLER_H

#include <stddef.h>
#include <stdint.h>

typedef void (*LocalHttpResponseAttemptCallback)(bool sent, void *context);

struct LocalHttpResponse {
    const char *status;
    const char *contentType;
    size_t bodyLength;
    LocalHttpResponseAttemptCallback afterAttempt;
    void *afterAttemptContext;
    bool allowUnreadRequestBody;
    const char *body;
};

struct LocalHttpRequest {
    const char *requestLine;
    uint32_t peerAddress;
    uint32_t networkGeneration;
    const char *authorization;
    uint8_t authorizationCount;
    const char *contentType;
    uint8_t contentTypeCount;
    const char *host;
    uint8_t hostCount;
    const char *origin;
    uint8_t originCount;
    bool hasCookie;
    bool bodyFramingValid;
    bool hasContentLength;
    bool hasTransferEncoding;
    size_t contentLength;
    size_t prefetchedLength;
};

class LocalHttpHandler {
public:
    virtual ~LocalHttpHandler() {}
    virtual LocalHttpResponse handle(
        const char *requestLine,
        char *body,
        size_t bodySize) = 0;

    virtual LocalHttpResponse handleRequest(
        const LocalHttpRequest &request,
        char *body,
        size_t bodySize) {
        return handle(request.requestLine, body, bodySize);
    }

    virtual bool requiresRequestMetadata(const char *requestLine) {
        (void)requestLine;
        return false;
    }
};

#endif