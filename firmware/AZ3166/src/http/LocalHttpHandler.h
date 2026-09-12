#ifndef LOCAL_HTTP_HANDLER_H
#define LOCAL_HTTP_HANDLER_H

#include <stddef.h>

struct LocalHttpResponse {
    const char *status;
    const char *contentType;
    size_t bodyLength;
};

class LocalHttpHandler {
public:
    virtual ~LocalHttpHandler() {}
    virtual LocalHttpResponse handle(
        const char *requestLine,
        char *body,
        size_t bodySize) = 0;
};

#endif