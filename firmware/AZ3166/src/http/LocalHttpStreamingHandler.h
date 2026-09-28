#ifndef LOCAL_HTTP_STREAMING_HANDLER_H
#define LOCAL_HTTP_STREAMING_HANDLER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "LocalHttpHandler.h"

enum LocalHttpBodyReadStatus {
    LOCAL_HTTP_BODY_DATA,
    LOCAL_HTTP_BODY_COMPLETE,
    LOCAL_HTTP_BODY_DISCONNECTED,
    LOCAL_HTTP_BODY_TIMEOUT,
    LOCAL_HTTP_BODY_CANCELLED,
    LOCAL_HTTP_BODY_ERROR
};

enum LocalHttpRequestMetadataStatus {
    LOCAL_HTTP_METADATA_ABSENT,
    LOCAL_HTTP_METADATA_VALID,
    LOCAL_HTTP_METADATA_DUPLICATE,
    LOCAL_HTTP_METADATA_MALFORMED,
    LOCAL_HTTP_METADATA_TOO_LONG
};

struct LocalHttpHeaderMetadata {
    LocalHttpRequestMetadataStatus status;
    const char *value;
    size_t length;
};

enum LocalHttpPeerIpv4Status {
    LOCAL_HTTP_PEER_IPV4_UNAVAILABLE,
    LOCAL_HTTP_PEER_IPV4_VALID
};

struct LocalHttpPeerIpv4Metadata {
    LocalHttpPeerIpv4Status status;
    uint32_t address;
};

struct LocalHttpStreamingRequest {
    static const size_t AUTHORIZATION_CAPACITY = 64;
    static const size_t HOST_CAPACITY = 128;
    static const size_t ORIGIN_CAPACITY = 256;

    const char *requestLine;
    size_t contentLength;
    uint32_t generation;
    LocalHttpHeaderMetadata authorization;
    LocalHttpHeaderMetadata host;
    LocalHttpHeaderMetadata origin;
    LocalHttpPeerIpv4Metadata peerIpv4;
    LocalHttpRequest metadata;
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
