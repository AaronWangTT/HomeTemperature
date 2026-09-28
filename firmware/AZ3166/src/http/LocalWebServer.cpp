#include "Az3166LocalWebServerOperations.h"

#include <mutex>
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include "lwip/sockets.h"

namespace {

const uint32_t IO_TIMEOUT_MS = 2000UL;
const uint32_t WORKER_STACK_SIZE = 6144;
const uint32_t STREAMING_WORKER_STACK_SIZE = 6144;
const size_t REQUEST_LINE_SIZE = 96;
const size_t RESPONSE_BODY_SIZE = 512;
const size_t MAX_HEADER_BYTES = 2048;

bool asciiEqualIgnoreCase(const char *value, size_t length, const char *expected) {
    size_t expectedLength = strlen(expected);
    if (length != expectedLength) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        char current = value[index];
        if (current >= 'A' && current <= 'Z') {
            current = static_cast<char>(current - 'A' + 'a');
        }
        if (current != expected[index]) {
            return false;
        }
    }
    return true;
}

bool parseContentLength(const char *value, size_t length, size_t &result) {
    while (length > 0 && (*value == ' ' || *value == '\t')) {
        ++value;
        --length;
    }
    while (length > 0 &&
           (value[length - 1] == ' ' || value[length - 1] == '\t')) {
        --length;
    }
    if (length == 0) {
        return false;
    }

    size_t parsed = 0;
    for (size_t index = 0; index < length; ++index) {
        char current = value[index];
        if (current < '0' || current > '9') {
            return false;
        }
        size_t digit = static_cast<size_t>(current - '0');
        if (parsed > (SIZE_MAX - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    result = parsed;
    return true;
}

bool setNonblocking(int descriptor) {
    unsigned long enabled = 1;
    return lwip_ioctl(descriptor, FIONBIO, &enabled) == 0;
}

int socketReady(int descriptor, bool writing) {
    if (descriptor < 0 || descriptor >= FD_SETSIZE) {
        return -1;
    }
    fd_set descriptors;
    FD_ZERO(&descriptors);
    FD_SET(descriptor, &descriptors);
    timeval timeout = {};
    return lwip_select(descriptor + 1,
                       writing ? NULL : &descriptors,
                       writing ? &descriptors : NULL, NULL, &timeout);
}

}  // namespace

uint32_t Az3166LocalWebServerOperations::currentTime() {
    return millis();
}

void Az3166LocalWebServerOperations::closeSocket(int descriptor) {
    lwip_close(descriptor);
}

int Az3166LocalWebServerOperations::listenerReady(int listener) {
    return socketReady(listener, false);
}

int Az3166LocalWebServerOperations::acceptSocket(int listener) {
    return lwip_accept(listener, NULL, NULL);
}

int Az3166LocalWebServerOperations::getSocketOption(
    int descriptor, int level, int option, void *value, socklen_t *length) {
    return lwip_getsockopt(descriptor, level, option, value, length);
}

int Az3166LocalWebServerOperations::openListener(uint32_t address, uint16_t port) {
    LocalHttpSocket descriptor(*this, lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!descriptor) {
        return -1;
    }
    sockaddr_in local = {};
    local.sin_len = sizeof(local);
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = htonl(address);
    int reuse = 1;
    if (lwip_setsockopt(descriptor.get(), SOL_SOCKET, SO_REUSEADDR,
                       &reuse, sizeof(reuse)) != 0 ||
        !setNonblocking(descriptor.get()) ||
        lwip_bind(descriptor.get(), reinterpret_cast<sockaddr *>(&local),
                  sizeof(local)) != 0 ||
        lwip_listen(descriptor.get(), 2) != 0) {
        return -1;
    }
    return descriptor.release();
}

int Az3166LocalWebServerOperations::acceptClient(int listener) {
    int ready = listenerReady(listener);
    if (ready == 0) {
        return LocalWebServer::ACCEPT_IDLE;
    }
    if (ready < 0) {
        return LocalWebServer::ACCEPT_ERROR;
    }
    LocalHttpSocket client(*this, acceptSocket(listener));
    if (!client) {
        int socketError = 0;
        socklen_t errorSize = sizeof(socketError);
        if (getSocketOption(listener, SOL_SOCKET, SO_ERROR,
                    &socketError, &errorSize) != 0 ||
            errorSize != sizeof(socketError)) {
            return LocalWebServer::ACCEPT_ERROR;
        }
        return LocalWebServer::classifyAcceptError(socketError);
    }
    int noDelay = 1;
    if (!setNonblocking(client.get()) ||
        lwip_setsockopt(client.get(), IPPROTO_TCP, TCP_NODELAY,
                       &noDelay, sizeof(noDelay)) != 0) {
        return LocalWebServer::ACCEPT_IDLE;
    }
    return client.release();
}

int Az3166LocalWebServerOperations::receiveBytes(int client, char *buffer, size_t size) {
    int ready = socketReady(client, false);
    if (ready == 0) {
        return 0;
    }
    if (ready < 0) {
        return LocalWebServer::RECEIVE_ERROR;
    }
    int received = lwip_recv(client, buffer, size, MSG_DONTWAIT);
    if (received > 0) {
        return received;
    }
    if (received == 0) {
        return LocalWebServer::RECEIVE_DISCONNECTED;
    }
    if (errno == LWIP_EAGAIN || errno == LWIP_EWOULDBLOCK ||
        errno == LWIP_EINTR) {
        return 0;
    }
    return LocalWebServer::RECEIVE_ERROR;
}

int Az3166LocalWebServerOperations::sendBytes(int client, const char *buffer, size_t size) {
    int ready = socketReady(client, true);
    if (ready <= 0) {
        return ready;
    }
    return lwip_send(client, buffer, size, MSG_DONTWAIT);
}

int LocalWebServer::classifyAcceptError(int socketError) {
    bool transient = socketError == LWIP_EAGAIN ||
        socketError == LWIP_EWOULDBLOCK || socketError == LWIP_EINTR ||
        socketError == LWIP_ECONNABORTED || socketError == LWIP_ECONNRESET;
    return transient ? ACCEPT_IDLE : ACCEPT_ERROR;
}

LocalHttpSocket::LocalHttpSocket(LocalWebServerOperations &operations, int descriptor)
    : operations_(&operations), descriptor_(descriptor) {
}

LocalHttpSocket::~LocalHttpSocket() {
    reset();
}

LocalHttpSocket::LocalHttpSocket(LocalHttpSocket &&other) noexcept
    : operations_(other.operations_), descriptor_(other.release()) {
}

LocalHttpSocket &LocalHttpSocket::operator=(LocalHttpSocket &&other) noexcept {
    if (this != &other) {
        reset();
        operations_ = other.operations_;
        descriptor_ = other.release();
    }
    return *this;
}

int LocalHttpSocket::get() const noexcept {
    return descriptor_;
}

LocalHttpSocket::operator bool() const noexcept {
    return descriptor_ >= 0;
}

int LocalHttpSocket::release() noexcept {
    int descriptor = descriptor_;
    descriptor_ = -1;
    return descriptor;
}

void LocalHttpSocket::reset(int descriptor) noexcept {
    if (descriptor_ == descriptor) {
        return;
    }
    int previous = descriptor_;
    descriptor_ = descriptor;
    if (previous >= 0) {
        operations_->closeSocket(previous);
    }
}

class LocalHttpBodyStreamImpl : public LocalHttpBodyStream {
public:
    LocalHttpBodyStreamImpl(
        LocalWebServer &server,
        LocalWebServerOperations &operations,
        int client,
        uint32_t generation,
        size_t contentLength,
        const char *prefetched,
        size_t prefetchedLength,
        const LocalHttpStreamingLimits &limits)
        : server_(server),
          operations_(operations),
          client_(client),
          generation_(generation),
          remaining_(contentLength),
          prefetched_(prefetched),
          prefetchedLength_(prefetchedLength),
          prefetchedOffset_(0),
          started_(operations.currentTime()),
          lastProgress_(started_),
          limits_(limits) {
    }

    LocalHttpBodyReadStatus read(
        char *buffer,
        size_t capacity,
        size_t &received) override {
        received = 0;
        if (buffer == NULL || capacity == 0) {
            return LOCAL_HTTP_BODY_ERROR;
        }
        if (!server_.isStreamingCurrent(generation_)) {
            return LOCAL_HTTP_BODY_CANCELLED;
        }
        if (remaining_ == 0) {
            return LOCAL_HTTP_BODY_COMPLETE;
        }

        uint32_t now = operations_.currentTime();
        if (now - started_ >= limits_.totalTimeoutMs ||
            now - lastProgress_ >= limits_.idleTimeoutMs) {
            return LOCAL_HTTP_BODY_TIMEOUT;
        }

        if (prefetchedOffset_ < prefetchedLength_) {
            size_t available = prefetchedLength_ - prefetchedOffset_;
            received = available < capacity ? available : capacity;
            if (received > remaining_) {
                return LOCAL_HTTP_BODY_ERROR;
            }
            memcpy(buffer, prefetched_ + prefetchedOffset_, received);
            prefetchedOffset_ += received;
            remaining_ -= received;
            lastProgress_ = operations_.currentTime();
            return LOCAL_HTTP_BODY_DATA;
        }

        int count = operations_.receiveBytes(
            client_, buffer, capacity < remaining_ ? capacity : remaining_);
        if (count == LocalWebServer::RECEIVE_DISCONNECTED) {
            return LOCAL_HTTP_BODY_DISCONNECTED;
        }
        if (count < 0) {
            return LOCAL_HTTP_BODY_ERROR;
        }
        if (count == 0) {
            rtos::Thread::wait(1);
            return server_.isStreamingCurrent(generation_)
                ? LOCAL_HTTP_BODY_DATA
                : LOCAL_HTTP_BODY_CANCELLED;
        }
        if (static_cast<size_t>(count) > capacity ||
            static_cast<size_t>(count) > remaining_) {
            return LOCAL_HTTP_BODY_ERROR;
        }
        uint32_t completed = operations_.currentTime();
        if (completed - started_ >= limits_.totalTimeoutMs ||
            completed - lastProgress_ >= limits_.idleTimeoutMs) {
            return LOCAL_HTTP_BODY_TIMEOUT;
        }
        received = static_cast<size_t>(count);
        remaining_ -= received;
        lastProgress_ = completed;
        return LOCAL_HTTP_BODY_DATA;
    }

    size_t remaining() const override {
        return remaining_;
    }

    bool cancelled() const override {
        return !server_.isStreamingCurrent(generation_);
    }

private:
    LocalWebServer &server_;
    LocalWebServerOperations &operations_;
    int client_;
    uint32_t generation_;
    size_t remaining_;
    const char *prefetched_;
    size_t prefetchedLength_;
    size_t prefetchedOffset_;
    uint32_t started_;
    uint32_t lastProgress_;
    LocalHttpStreamingLimits limits_;
};

LocalWebServer::LocalWebServer(
    LocalHttpHandler &handler,
    uint16_t port,
    uint32_t startRetryIntervalMs,
    LocalHttpServiceUpdate serviceUpdate)
    : LocalWebServer(handler, port, startRetryIntervalMs, defaultOperations(),
                     serviceUpdate) {
}

LocalWebServer::LocalWebServer(
    LocalHttpHandler &handler,
    uint16_t port,
    uint32_t startRetryIntervalMs,
    LocalWebServerOperations &operations,
    LocalHttpServiceUpdate serviceUpdate)
    : handler_(handler),
      streamingHandler_(NULL),
      streamingLimits_({0, 0, 0}),
      port_(port),
      startRetryIntervalMs_(startRetryIntervalMs),
      operations_(operations),
      streamingOperations_(operations),
      serviceUpdate_(serviceUpdate),
      worker_(osPriorityNormal, WORKER_STACK_SIZE),
      streamingWorker_(osPriorityNormal, STREAMING_WORKER_STACK_SIZE),
      streamingSignal_(0),
      requested_({0, 0, false}),
      state_({false, false, 0, 0}),
      streamingJob_({false, false, false, -1, 0, 0, 0, {}, {}}),
      lastWorkerAttempt_(0),
      workerAttempted_(false),
      workerStarting_(false),
      streamingWorkerStarted_(false),
      streamingWorkerStarting_(false),
      streamingWorkerAttempted_(false),
      lastStreamingWorkerAttempt_(0) {
}

LocalWebServer::LocalWebServer(
    LocalHttpHandler &handler,
    LocalHttpStreamingHandler &streamingHandler,
    const LocalHttpStreamingLimits &streamingLimits,
    uint16_t port,
    uint32_t startRetryIntervalMs,
    LocalHttpServiceUpdate serviceUpdate)
    : LocalWebServer(handler, streamingHandler, streamingLimits, port,
                     startRetryIntervalMs, defaultOperations(),
                     defaultOperations(), serviceUpdate) {
}

LocalWebServer::LocalWebServer(
    LocalHttpHandler &handler,
    LocalHttpStreamingHandler &streamingHandler,
    const LocalHttpStreamingLimits &streamingLimits,
    uint16_t port,
    uint32_t startRetryIntervalMs,
    LocalWebServerOperations &operations,
    LocalHttpServiceUpdate serviceUpdate)
    : LocalWebServer(handler, streamingHandler, streamingLimits, port,
                     startRetryIntervalMs, operations, operations,
                     serviceUpdate) {
}

LocalWebServer::LocalWebServer(
    LocalHttpHandler &handler,
    LocalHttpStreamingHandler &streamingHandler,
    const LocalHttpStreamingLimits &streamingLimits,
    uint16_t port,
    uint32_t startRetryIntervalMs,
    LocalWebServerOperations &listenerOperations,
    LocalWebServerOperations &streamingOperations,
    LocalHttpServiceUpdate serviceUpdate)
    : handler_(handler),
      streamingHandler_(&streamingHandler),
      streamingLimits_(streamingLimits),
      port_(port),
      startRetryIntervalMs_(startRetryIntervalMs),
      operations_(listenerOperations),
      streamingOperations_(streamingOperations),
      serviceUpdate_(serviceUpdate),
      worker_(osPriorityNormal, WORKER_STACK_SIZE),
      streamingWorker_(osPriorityNormal, STREAMING_WORKER_STACK_SIZE),
      streamingSignal_(0),
      requested_({0, 0, false}),
      state_({false, false, 0, 0}),
      streamingJob_({false, false, false, -1, 0, 0, 0, {}, {}}),
      lastWorkerAttempt_(0),
      workerAttempted_(false),
      workerStarting_(false),
      streamingWorkerStarted_(false),
      streamingWorkerStarting_(false),
      streamingWorkerAttempted_(false),
      lastStreamingWorkerAttempt_(0) {
}

LocalWebServer::~LocalWebServer() {
    bool started;
    bool streamingStarted;
    {
        std::lock_guard<rtos::Mutex> lock(stateMutex_);
        requested_.shutdown = true;
        started = state_.workerStarted;
        streamingStarted = streamingWorkerStarted_;
    }
    {
        std::lock_guard<rtos::Mutex> lock(streamingMutex_);
        streamingJob_.cancelled = true;
    }
    streamingSignal_.release();
    if (started) {
        worker_.join();
    }
    if (streamingStarted) {
        streamingWorker_.join();
    }
}

LocalWebServerOperations &LocalWebServer::defaultOperations() {
    static Az3166LocalWebServerOperations operations;
    return operations;
}

bool LocalWebServer::startStreamingWorker(uint32_t now) {
    if (streamingHandler_ == NULL) {
        return true;
    }
    {
        std::lock_guard<rtos::Mutex> lock(stateMutex_);
        if (streamingWorkerStarted_) {
            return true;
        }
        if (streamingWorkerStarting_) {
            return false;
        }
        if (streamingLimits_.maxContentLength == 0 ||
            streamingLimits_.idleTimeoutMs == 0 ||
            streamingLimits_.totalTimeoutMs < streamingLimits_.idleTimeoutMs ||
            (&operations_ == &streamingOperations_ &&
             !operations_.supportsConcurrentSockets())) {
            state_.error = ACCEPT_ERROR;
            return false;
        }
        if (streamingWorkerAttempted_ &&
            now - lastStreamingWorkerAttempt_ < startRetryIntervalMs_) {
            return false;
        }
        streamingWorkerStarting_ = true;
        streamingWorkerAttempted_ = true;
    }

    osStatus result =
        streamingWorker_.start(mbed::callback(this, &LocalWebServer::runStreaming));
    {
        std::lock_guard<rtos::Mutex> lock(stateMutex_);
        streamingWorkerStarting_ = false;
        streamingWorkerStarted_ = result == osOK;
        if (result != osOK) {
            state_.error = static_cast<int>(result);
            lastStreamingWorkerAttempt_ = operations_.currentTime();
        }
    }
    if (result != osOK) {
        Serial.println("Local HTTP streaming worker start failed");
    }
    return result == osOK;
}

void LocalWebServer::update(bool wifiConnected, uint32_t address) {
    uint32_t now = operations_.currentTime();
    uint32_t startupGeneration;
    uint32_t requestedAddress = wifiConnected ? address : 0;
    {
        std::lock_guard<rtos::Mutex> lock(stateMutex_);
        if (requested_.address != requestedAddress) {
            requested_.address = requestedAddress;
            ++requested_.generation;
            state_.listening = false;
            state_.address = 0;
            state_.error = 0;
            workerAttempted_ = false;
        }
        if (requested_.shutdown || requestedAddress == 0 || port_ == 0) {
            return;
        }
    }

    if (!startStreamingWorker(now)) {
        return;
    }

    {
        std::lock_guard<rtos::Mutex> lock(stateMutex_);
        if (requested_.shutdown || requested_.address != requestedAddress ||
            state_.workerStarted || workerStarting_ ||
            (workerAttempted_ && now - lastWorkerAttempt_ < startRetryIntervalMs_)) {
            return;
        }
        workerStarting_ = true;
        workerAttempted_ = true;
        startupGeneration = requested_.generation;
        state_.error = 0;
    }

    osStatus result = worker_.start(mbed::callback(this, &LocalWebServer::run));
    uint32_t completed = operations_.currentTime();
    {
        std::lock_guard<rtos::Mutex> lock(stateMutex_);
        workerStarting_ = false;
        if (result == osOK) {
            state_.workerStarted = true;
        } else if (!requested_.shutdown && requested_.generation == startupGeneration) {
            state_.error = static_cast<int>(result);
            lastWorkerAttempt_ = completed;
        }
    }
    if (result != osOK) {
        Serial.println("Local HTTP worker start failed; retrying later");
    }
}

LocalWebServerState LocalWebServer::state() const {
    std::lock_guard<rtos::Mutex> lock(stateMutex_);
    return state_;
}

LocalWebServer::RequestedState LocalWebServer::requestedState() const {
    std::lock_guard<rtos::Mutex> lock(stateMutex_);
    return requested_;
}

bool LocalWebServer::isCurrent(uint32_t generation) const {
    RequestedState snapshot = requestedState();
    return !snapshot.shutdown && snapshot.address != 0 &&
        snapshot.generation == generation;
}

bool LocalWebServer::isStreamingCurrent(uint32_t generation) const {
    if (!isCurrent(generation)) {
        return false;
    }
    std::lock_guard<rtos::Mutex> lock(streamingMutex_);
    return (streamingJob_.pending || streamingJob_.active) &&
        streamingJob_.generation == generation && !streamingJob_.cancelled;
}

bool LocalWebServer::cancelStreamingRequest(uint32_t generation) {
    std::lock_guard<rtos::Mutex> lock(streamingMutex_);
    if ((!streamingJob_.pending && !streamingJob_.active) ||
        streamingJob_.generation != generation) {
        return false;
    }
    streamingJob_.cancelled = true;
    streamingSignal_.release();
    return true;
}

bool LocalWebServer::publishState(uint32_t generation, uint32_t address, int error) {
    std::lock_guard<rtos::Mutex> lock(stateMutex_);
    bool current = !requested_.shutdown && requested_.generation == generation;
    if (current) {
        state_.listening = address != 0;
        state_.address = address;
        state_.error = error;
    }
    return current;
}

void LocalWebServer::notifyService(bool available, uint32_t address) {
    if (serviceUpdate_) {
        serviceUpdate_(available, address);
    }
}

void LocalWebServer::run() {
    {
        std::lock_guard<rtos::Mutex> lock(stateMutex_);
        state_.workerStarted = true;
    }
    LocalHttpSocket listener(operations_);
    uint32_t generation = 0;
    uint32_t lastAttempt = 0;
    bool attempted = false;

    for (;;) {
        RequestedState requested = requestedState();
        if (requested.shutdown) {
            break;
        }
        if (generation != requested.generation) {
            notifyService(false, 0);
            listener.reset();
            generation = requested.generation;
            attempted = false;
        }

        if (requested.address != 0 && !listener &&
            (!attempted || operations_.currentTime() - lastAttempt >=
                startRetryIntervalMs_)) {
            listener.reset(operations_.openListener(requested.address, port_));
            attempted = true;
            lastAttempt = operations_.currentTime();
            if (!publishState(generation, listener ? requested.address : 0,
                              listener ? 0 : listener.get())) {
                listener.reset();
                continue;
            }
            if (!listener) {
                Serial.println("Local HTTP listener start failed; retrying later");
            }
        }

        if (listener && isCurrent(generation)) {
            notifyService(true, requested.address);
            LocalHttpSocket client(operations_, operations_.acceptClient(listener.get()));
            if (client) {
                serveClient(client, generation);
            } else if (client.get() == ACCEPT_ERROR) {
                notifyService(false, 0);
                listener.reset();
                publishState(generation, 0, ACCEPT_ERROR);
                lastAttempt = operations_.currentTime();
            }
        } else {
            notifyService(false, 0);
        }
        rtos::Thread::wait(20);
    }

    notifyService(false, 0);
}

void LocalWebServer::runStreaming() {
    for (;;) {
        streamingSignal_.wait(20);

        RequestedState requested = requestedState();
        int client = -1;
        uint32_t generation = 0;
        size_t contentLength = 0;
        size_t prefetchedLength = 0;
        char requestLine[REQUEST_LINE_SIZE];
        char prefetched[MAX_PREFETCH_BYTES];
        {
            std::lock_guard<rtos::Mutex> lock(streamingMutex_);
            if (!streamingJob_.pending) {
                if (requested.shutdown && !streamingJob_.active) {
                    break;
                }
                continue;
            }
            streamingJob_.pending = false;
            streamingJob_.active = true;
            client = streamingJob_.client;
            streamingJob_.client = -1;
            generation = streamingJob_.generation;
            contentLength = streamingJob_.contentLength;
            prefetchedLength = streamingJob_.prefetchedLength;
            memcpy(requestLine, streamingJob_.requestLine, sizeof(requestLine));
            memcpy(prefetched, streamingJob_.prefetched, prefetchedLength);
        }

        LocalHttpSocket socket(streamingOperations_, client);
        if (isStreamingCurrent(generation)) {
            LocalHttpBodyStreamImpl body(
                *this, streamingOperations_, socket.get(), generation,
                contentLength, prefetched, prefetchedLength, streamingLimits_);
            LocalHttpStreamingRequest request = {
                requestLine, contentLength, generation
            };
            char responseBody[RESPONSE_BODY_SIZE] = {};
            LocalHttpResponse response = streamingHandler_->handle(
                request, body, responseBody, sizeof(responseBody));
            if (body.remaining() != 0) {
                strcpy(responseBody, "{\"error\":\"incomplete request body\"}");
                response = {
                    "400 Bad Request", "application/json", strlen(responseBody)
                };
            } else if (response.bodyLength > sizeof(responseBody) ||
                       response.status == NULL || response.contentType == NULL) {
                strcpy(responseBody, "{\"error\":\"invalid response\"}");
                response = {
                    "500 Internal Server Error", "application/json",
                    strlen(responseBody)
                };
            }
            bool sent = sendResponse(
                streamingOperations_, socket.get(), response, responseBody,
                generation, true);
            Serial.print("Local HTTP streaming response: ");
            Serial.println(sent ? response.status : "send failed or session changed");
        }

        {
            std::lock_guard<rtos::Mutex> lock(streamingMutex_);
            if (streamingJob_.generation == generation) {
                streamingJob_.active = false;
                streamingJob_.cancelled = false;
                streamingJob_.generation = 0;
            }
        }
    }
}

void LocalWebServer::serveClient(LocalHttpSocket &client, uint32_t generation) {
    ParsedRequest request = {};
    char body[RESPONSE_BODY_SIZE] = {};
    LocalHttpResponse response;
    if (!readRequest(client.get(), request, generation)) {
        strcpy(body, "{\"error\":\"bad request\"}");
        response = {"400 Bad Request", "application/json", strlen(body)};
    } else if (streamingHandler_ != NULL &&
               streamingHandler_->handles(request.requestLine)) {
        if (!request.bodyFramingValid || request.hasTransferEncoding ||
            !request.hasContentLength ||
            request.contentLength > streamingLimits_.maxContentLength ||
            request.prefetchedLength > request.contentLength) {
            strcpy(body, "{\"error\":\"invalid content length\"}");
            response = {"400 Bad Request", "application/json", strlen(body)};
        } else if (transferStreamingRequest(client, request, generation)) {
            return;
        } else {
            strcpy(body, "{\"error\":\"streaming request busy\"}");
            response = {
                "503 Service Unavailable", "application/json", strlen(body)
            };
        }
    } else {
        response = handler_.handle(request.requestLine, body, sizeof(body));
        if (response.bodyLength > sizeof(body) || response.status == NULL ||
            response.contentType == NULL) {
            strcpy(body, "{\"error\":\"invalid response\"}");
            response = {"500 Internal Server Error", "application/json", strlen(body)};
        }
    }
    bool sent = sendResponse(
        operations_, client.get(), response, body, generation, false);
    Serial.print("Local HTTP response: ");
    Serial.println(sent ? response.status : "send failed or session changed");
}

bool LocalWebServer::readRequest(
    int client,
    ParsedRequest &request,
    uint32_t generation) {
    uint32_t requestStart = operations_.currentTime();
    char receivedBytes[MAX_HEADER_BYTES + MAX_PREFETCH_BYTES];
    size_t receivedLength = 0;
    size_t headerLength = 0;
    size_t requestLineLength = 0;
    bool requestLineComplete = false;
    bool requestLineCarriageReturn = false;
    bool canonicalFraming = true;
    bool previousCarriageReturn = false;

    while (isCurrent(generation) &&
           operations_.currentTime() - requestStart < IO_TIMEOUT_MS) {
        char buffer[128];
        int received = operations_.receiveBytes(client, buffer, sizeof(buffer));
        if (received < 0 || received > static_cast<int>(sizeof(buffer))) {
            return false;
        }

        for (int offset = 0; offset < received; ++offset) {
            char current = buffer[offset];
            if (current == '\0' || receivedLength >= sizeof(receivedBytes)) {
                return false;
            }
            receivedBytes[receivedLength++] = current;
            if ((previousCarriageReturn && current != '\n') ||
                (current == '\n' && !previousCarriageReturn)) {
                canonicalFraming = false;
            }
            previousCarriageReturn = current == '\r';
            if (!requestLineComplete) {
                if (current == '\n') {
                    requestLineComplete = true;
                    requestLineCarriageReturn = false;
                } else if (requestLineCarriageReturn) {
                    return false;
                } else if (current == '\r') {
                    requestLineCarriageReturn = true;
                } else if (requestLineLength + 1 >= REQUEST_LINE_SIZE) {
                    return false;
                } else {
                    ++requestLineLength;
                }
            }
            bool canonicalTerminator = receivedLength >= 4 &&
                memcmp(receivedBytes + receivedLength - 4, "\r\n\r\n", 4) == 0;
            bool legacyTerminator = receivedLength >= 2 &&
                memcmp(receivedBytes + receivedLength - 2, "\n\n", 2) == 0;
            if (canonicalTerminator || legacyTerminator) {
                canonicalFraming = canonicalFraming && canonicalTerminator;
                headerLength = receivedLength;
                size_t trailing = static_cast<size_t>(received - offset - 1);
                if (trailing > MAX_PREFETCH_BYTES) {
                    return false;
                }
                memcpy(request.prefetched, buffer + offset + 1, trailing);
                request.prefetchedLength = trailing;
                break;
            }
        }
        if (headerLength != 0) {
            break;
        }
        if (receivedLength > MAX_HEADER_BYTES) {
            return false;
        }
        rtos::Thread::wait(1);
    }

    if (headerLength == 0 || headerLength > MAX_HEADER_BYTES) {
        return false;
    }

    size_t firstLineNext = 0;
    for (size_t index = 0; index < headerLength; ++index) {
        if (receivedBytes[index] == '\n') {
            firstLineNext = index + 1;
            break;
        }
    }
    if (firstLineNext == 0) {
        return false;
    }
    requestLineLength = firstLineNext - 1;
    if (requestLineLength > 0 &&
        receivedBytes[requestLineLength - 1] == '\r') {
        --requestLineLength;
    }
    if (requestLineLength == 0 || requestLineLength >= sizeof(request.requestLine)) {
        return false;
    }
    memcpy(request.requestLine, receivedBytes, requestLineLength);
    request.requestLine[requestLineLength] = '\0';

    request.bodyFramingValid = canonicalFraming;
    request.hasContentLength = false;
    request.hasTransferEncoding = false;
    request.contentLength = 0;
    if (!canonicalFraming) {
        return true;
    }

    size_t position = firstLineNext;
    size_t headersEnd = headerLength - 2;
    while (position < headersEnd) {
        size_t lineEnd = position;
        while (lineEnd + 1 < headerLength &&
               !(receivedBytes[lineEnd] == '\r' &&
                 receivedBytes[lineEnd + 1] == '\n')) {
            ++lineEnd;
        }
        if (lineEnd + 1 >= headerLength || lineEnd == position) {
            request.bodyFramingValid = false;
            break;
        }

        size_t colon = position;
        while (colon < lineEnd && receivedBytes[colon] != ':') {
            unsigned char current =
                static_cast<unsigned char>(receivedBytes[colon]);
            if (current <= 32 || current >= 127) {
                request.bodyFramingValid = false;
            }
            ++colon;
        }
        if (colon == position || colon == lineEnd) {
            request.bodyFramingValid = false;
            position = lineEnd + 2;
            continue;
        }
        const char *name = receivedBytes + position;
        size_t nameLength = colon - position;
        const char *value = receivedBytes + colon + 1;
        size_t valueLength = lineEnd - colon - 1;
        if (asciiEqualIgnoreCase(name, nameLength, "transfer-encoding")) {
            request.hasTransferEncoding = true;
        }
        if (asciiEqualIgnoreCase(name, nameLength, "content-length")) {
            if (request.hasContentLength ||
                !parseContentLength(value, valueLength, request.contentLength)) {
                request.bodyFramingValid = false;
            }
            request.hasContentLength = true;
        }
        position = lineEnd + 2;
    }
    return position == headersEnd;
}

bool LocalWebServer::transferStreamingRequest(
    LocalHttpSocket &client,
    const ParsedRequest &request,
    uint32_t generation) {
    if (!isCurrent(generation)) {
        return false;
    }
    {
        std::lock_guard<rtos::Mutex> lock(streamingMutex_);
        if (streamingJob_.pending || streamingJob_.active) {
            return false;
        }
        streamingJob_.pending = true;
        streamingJob_.cancelled = false;
        streamingJob_.client = client.release();
        streamingJob_.generation = generation;
        streamingJob_.contentLength = request.contentLength;
        streamingJob_.prefetchedLength = request.prefetchedLength;
        memcpy(streamingJob_.requestLine, request.requestLine,
               sizeof(streamingJob_.requestLine));
        memcpy(streamingJob_.prefetched, request.prefetched,
               request.prefetchedLength);
    }
    streamingSignal_.release();
    return true;
}

bool LocalWebServer::sendResponse(
    LocalWebServerOperations &operations,
    int client,
    const LocalHttpResponse &response,
    const char *body,
    uint32_t generation,
    bool streaming) {
    char header[256];
    int length = snprintf(
        header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
        "Connection: close\r\n\r\n",
        response.status, response.contentType,
        static_cast<unsigned int>(response.bodyLength));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(header)) {
        return false;
    }
    uint32_t started = operations.currentTime();
    return sendAll(operations, client, header, static_cast<size_t>(length),
                   generation, started, streaming) &&
        sendAll(operations, client, body, response.bodyLength,
                generation, started, streaming);
}

bool LocalWebServer::sendAll(
    LocalWebServerOperations &operations,
    int client, const char *buffer, size_t size,
    uint32_t generation, uint32_t started, bool streaming) {
    size_t sent = 0;
    while (sent < size &&
           (streaming ? isStreamingCurrent(generation) : isCurrent(generation)) &&
           operations.currentTime() - started < IO_TIMEOUT_MS) {
        int count = operations.sendBytes(client, buffer + sent, size - sent);
        if (count < 0 || static_cast<size_t>(count) > size - sent) {
            return false;
        }
        sent += static_cast<size_t>(count);
        if (count == 0) {
            rtos::Thread::wait(1);
        }
    }
    return sent == size;
}