#ifndef LOCAL_WEB_SERVER_H
#define LOCAL_WEB_SERVER_H

#include <stdint.h>
#include "platform/Callback.h"
#include "rtos.h"
#include "LocalHttpHandler.h"
#include "LocalHttpStreamingHandler.h"

class LocalWebServerOperations {
public:
    virtual ~LocalWebServerOperations() = default;

    virtual uint32_t currentTime() = 0;
    virtual int openListener(uint32_t address, uint16_t port) = 0;
    virtual int acceptClient(int listener) = 0;
    virtual int receiveBytes(int client, char *buffer, size_t size) = 0;
    virtual int sendBytes(int client, const char *buffer, size_t size) = 0;
    virtual bool peerIpv4(int client, uint32_t &address) {
        (void)client;
        address = 0;
        return false;
    }
    virtual void closeSocket(int descriptor) = 0;
    virtual bool supportsConcurrentSockets() const { return false; }
};

class LocalHttpSocket {
public:
    explicit LocalHttpSocket(LocalWebServerOperations &operations, int descriptor = -1);
    ~LocalHttpSocket();

    LocalHttpSocket(const LocalHttpSocket &) = delete;
    LocalHttpSocket &operator=(const LocalHttpSocket &) = delete;
    LocalHttpSocket(LocalHttpSocket &&other) noexcept;
    LocalHttpSocket &operator=(LocalHttpSocket &&other) noexcept;

    int get() const noexcept;
    explicit operator bool() const noexcept;
    int release() noexcept;
    void reset(int descriptor = -1) noexcept;

private:
    LocalWebServerOperations *operations_;
    int descriptor_;
};

struct LocalWebServerState {
    bool workerStarted;
    bool listening;
    uint32_t address;
    uint32_t generation;
    int error;
};

using LocalHttpServiceUpdate = mbed::Callback<void(bool, uint32_t)>;

class LocalWebServer {
public:
    static const int ACCEPT_IDLE = -1;
    static const int ACCEPT_ERROR = -2;
    static const int RECEIVE_DISCONNECTED = -1;
    static const int RECEIVE_ERROR = -2;
    static const size_t MAX_PREFETCH_BYTES = 128;

    static int classifyAcceptError(int socketError);

    LocalWebServer(
        LocalHttpHandler &handler,
        uint16_t port,
        uint32_t startRetryIntervalMs,
        LocalHttpServiceUpdate serviceUpdate = LocalHttpServiceUpdate());

    LocalWebServer(
        LocalHttpHandler &handler,
        uint16_t port,
        uint32_t startRetryIntervalMs,
        LocalWebServerOperations &operations,
        LocalHttpServiceUpdate serviceUpdate = LocalHttpServiceUpdate());

    LocalWebServer(
        LocalHttpHandler &handler,
        LocalHttpStreamingHandler &streamingHandler,
        const LocalHttpStreamingLimits &streamingLimits,
        uint16_t port,
        uint32_t startRetryIntervalMs,
        LocalHttpServiceUpdate serviceUpdate = LocalHttpServiceUpdate());

    LocalWebServer(
        LocalHttpHandler &handler,
        LocalHttpStreamingHandler &streamingHandler,
        const LocalHttpStreamingLimits &streamingLimits,
        uint16_t port,
        uint32_t startRetryIntervalMs,
        LocalWebServerOperations &operations,
        LocalHttpServiceUpdate serviceUpdate = LocalHttpServiceUpdate());

    LocalWebServer(
        LocalHttpHandler &handler,
        LocalHttpStreamingHandler &streamingHandler,
        const LocalHttpStreamingLimits &streamingLimits,
        uint16_t port,
        uint32_t startRetryIntervalMs,
        LocalWebServerOperations &listenerOperations,
        LocalWebServerOperations &streamingOperations,
        LocalHttpServiceUpdate serviceUpdate = LocalHttpServiceUpdate());

    ~LocalWebServer();

    void update(bool wifiConnected, uint32_t address);
    LocalWebServerState state() const;
    bool cancelStreamingRequest(uint32_t generation);

private:
    friend class LocalHttpBodyStreamImpl;

    struct RequestedState {
        uint32_t address;
        uint32_t generation;
        bool shutdown;
    };

    struct ParsedRequest {
        char requestLine[96];
        bool bodyFramingValid;
        bool hasContentLength;
        bool hasTransferEncoding;
        size_t contentLength;
        size_t prefetchedLength;
        char prefetched[MAX_PREFETCH_BYTES];
        uint8_t authorizationCount;
        LocalHttpRequestMetadataStatus authorizationStatus;
        size_t authorizationLength;
        char authorization[LocalHttpStreamingRequest::AUTHORIZATION_CAPACITY];
        uint8_t contentTypeCount;
        char contentType[48];
        uint8_t hostCount;
        LocalHttpRequestMetadataStatus hostStatus;
        size_t hostLength;
        char host[LocalHttpStreamingRequest::HOST_CAPACITY];
        uint8_t originCount;
        LocalHttpRequestMetadataStatus originStatus;
        size_t originLength;
        char origin[LocalHttpStreamingRequest::ORIGIN_CAPACITY];
        bool hasCookie;
        uint8_t ifNoneMatchCount;
        char ifNoneMatch[96];
    };

    struct StreamingJob {
        bool pending;
        bool active;
        bool cancelled;
        int client;
        uint32_t generation;
        size_t contentLength;
        size_t prefetchedLength;
        char requestLine[96];
        char prefetched[MAX_PREFETCH_BYTES];
        uint8_t authorizationCount;
        LocalHttpRequestMetadataStatus authorizationStatus;
        size_t authorizationLength;
        char authorization[LocalHttpStreamingRequest::AUTHORIZATION_CAPACITY];
        uint8_t contentTypeCount;
        char contentType[48];
        uint8_t hostCount;
        LocalHttpRequestMetadataStatus hostStatus;
        size_t hostLength;
        char host[LocalHttpStreamingRequest::HOST_CAPACITY];
        uint8_t originCount;
        LocalHttpRequestMetadataStatus originStatus;
        size_t originLength;
        char origin[LocalHttpStreamingRequest::ORIGIN_CAPACITY];
        bool hasCookie;
        LocalHttpPeerIpv4Metadata peerIpv4;
    };

    LocalWebServer(const LocalWebServer &) = delete;
    LocalWebServer &operator=(const LocalWebServer &) = delete;
    static LocalWebServerOperations &defaultOperations();
    bool startStreamingWorker(uint32_t now);
    RequestedState requestedState() const;
    bool isCurrent(uint32_t generation) const;
    bool isStreamingCurrent(uint32_t generation) const;
    bool publishState(uint32_t generation, uint32_t address, int error);
    void notifyService(bool available, uint32_t address);
    void run();
    void runStreaming();
    void serveClient(LocalHttpSocket &client, uint32_t generation);
    bool readRequest(
        int client,
        ParsedRequest &request,
        uint32_t generation);
    bool transferStreamingRequest(
        LocalHttpSocket &client,
        const ParsedRequest &request,
        uint32_t generation);
    bool sendResponse(
        LocalWebServerOperations &operations,
        int client,
        const LocalHttpResponse &response,
        const char *body,
        uint32_t generation,
        bool streaming);
    bool sendAll(LocalWebServerOperations &operations,
                 int client, const char *buffer, size_t size,
                 uint32_t generation, uint32_t started, bool streaming);

    LocalHttpHandler &handler_;
    LocalHttpStreamingHandler *streamingHandler_;
    LocalHttpStreamingLimits streamingLimits_;
    uint16_t port_;
    uint32_t startRetryIntervalMs_;
    LocalWebServerOperations &operations_;
    LocalWebServerOperations &streamingOperations_;
    LocalHttpServiceUpdate serviceUpdate_;
    mutable rtos::Mutex stateMutex_;
    mutable rtos::Mutex streamingMutex_;
    rtos::Thread worker_;
    rtos::Thread streamingWorker_;
    rtos::Semaphore streamingSignal_;
    RequestedState requested_;
    LocalWebServerState state_;
    StreamingJob streamingJob_;
    uint32_t lastWorkerAttempt_;
    bool workerAttempted_;
    bool workerStarting_;
    bool streamingWorkerStarted_;
    bool streamingWorkerStarting_;
    bool streamingWorkerAttempted_;
    uint32_t lastStreamingWorkerAttempt_;
};

#endif