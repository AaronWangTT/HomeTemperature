#ifndef LOCAL_HTTP_STREAMING_HANDLER_H
#define LOCAL_HTTP_STREAMING_HANDLER_H

#include <stddef.h>
#include <stdint.h>
#include "LocalHttpHandler.h"

enum LocalHttpBodyReadStatus {
    LOCAL_HTTP_BODY_DATA,
    LOCAL_HTTP_BODY_COMPLETE,
    LOCAL_HTTP_BODY_DISCONNECTED,
    LOCAL_HTTP_BODY_TIMEOUT,
    LOCAL_HTTP_BODY_CANCELLED,
    LOCAL_HTTP_BODY_ERROR
};

struct LocalHttpStreamingRequest {
    const char *requestLine;
    size_t contentLength;
    uint32_t generation;
};

class LocalHttpBodyStream {
public:
    virtual ~LocalHttpBodyStream() {}

    virtual LocalHttpBodyReadStatus read(
        char *buffer,
        size_t capacity,
        size_t &received) = 0;
    virtual size_t remaining() const = 0;
    virtual bool cancelled() const = 0;
};

class LocalHttpStreamingHandler {
public:
    virtual ~LocalHttpStreamingHandler() {}

    virtual bool handles(const char *requestLine) = 0;
    virtual LocalHttpResponse handle(
        const LocalHttpStreamingRequest &request,
        LocalHttpBodyStream &body,
        char *responseBody,
        size_t responseBodySize) = 0;
};

struct LocalHttpStreamingLimits {
    size_t maxContentLength;
    uint32_t idleTimeoutMs;
    uint32_t totalTimeoutMs;
};

#endif
