#include "LocalWebServer.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include "lwip/sockets.h"

namespace {

const uint32_t IO_TIMEOUT_MS = 2000UL;
const uint32_t WORKER_STACK_SIZE = 6144;
const size_t REQUEST_LINE_SIZE = 96;
const size_t RESPONSE_BODY_SIZE = 512;
const size_t MAX_HEADER_BYTES = 2048;

uint32_t platformCurrentTime() {
    return millis();
}

void platformCloseSocket(int descriptor) {
    lwip_close(descriptor);
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

int platformOpenListener(uint32_t address, uint16_t port) {
    int descriptor = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (descriptor < 0) {
        return -1;
    }
    sockaddr_in local = {};
    local.sin_len = sizeof(local);
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = htonl(address);
    int reuse = 1;
    if (lwip_setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR,
                       &reuse, sizeof(reuse)) != 0 ||
        !setNonblocking(descriptor) ||
        lwip_bind(descriptor, reinterpret_cast<sockaddr *>(&local),
                  sizeof(local)) != 0 ||
        lwip_listen(descriptor, 2) != 0) {
        platformCloseSocket(descriptor);
        return -1;
    }
    return descriptor;
}

int platformAcceptClient(int listener) {
    int ready = socketReady(listener, false);
    if (ready == 0) {
        return LocalWebServer::ACCEPT_IDLE;
    }
    if (ready < 0) {
        return LocalWebServer::ACCEPT_ERROR;
    }
    int client = lwip_accept(listener, NULL, NULL);
    if (client < 0) {
        return LocalWebServer::ACCEPT_ERROR;
    }
    int noDelay = 1;
    if (!setNonblocking(client) ||
        lwip_setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                       &noDelay, sizeof(noDelay)) != 0) {
        platformCloseSocket(client);
        return LocalWebServer::ACCEPT_IDLE;
    }
    return client;
}

int platformReceiveBytes(int client, char *buffer, size_t size) {
    int ready = socketReady(client, false);
    if (ready <= 0) {
        return ready;
    }
    int received = lwip_recv(client, buffer, size, MSG_DONTWAIT);
    return received > 0 ? received : -1;
}

int platformSendBytes(int client, const char *buffer, size_t size) {
    int ready = socketReady(client, true);
    if (ready <= 0) {
        return ready;
    }
    return lwip_send(client, buffer, size, MSG_DONTWAIT);
}

}  // namespace

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
    const LocalWebServerOperations &operations,
    LocalHttpServiceUpdate serviceUpdate)
    : handler_(handler),
      port_(port),
      startRetryIntervalMs_(startRetryIntervalMs),
      operations_(operations),
      serviceUpdate_(serviceUpdate),
      worker_(osPriorityNormal, WORKER_STACK_SIZE),
      requested_({0, 0, false}),
      state_({false, false, 0, 0}),
      lastWorkerAttempt_(0),
      workerAttempted_(false) {
}

LocalWebServer::~LocalWebServer() {
    stateMutex_.lock();
    requested_.shutdown = true;
    bool started = state_.workerStarted;
    stateMutex_.unlock();
    if (started) {
        worker_.join();
    }
}

LocalWebServerOperations LocalWebServer::defaultOperations() {
    LocalWebServerOperations operations = {
        platformCurrentTime, platformOpenListener, platformAcceptClient,
        platformReceiveBytes, platformSendBytes, platformCloseSocket
    };
    return operations;
}

void LocalWebServer::update(bool wifiConnected, uint32_t address) {
    stateMutex_.lock();
    uint32_t requestedAddress = wifiConnected ? address : 0;
    if (requested_.address != requestedAddress) {
        requested_.address = requestedAddress;
        ++requested_.generation;
        state_.listening = false;
        state_.address = 0;
        state_.error = 0;
        workerAttempted_ = false;
    }
    uint32_t now = operations_.currentTime();
    if (!state_.workerStarted && requestedAddress != 0 && port_ != 0 &&
        (!workerAttempted_ || now - lastWorkerAttempt_ >= startRetryIntervalMs_)) {
        osStatus result = worker_.start(mbed::callback(this, &LocalWebServer::run));
        state_.workerStarted = result == osOK;
        state_.error = result == osOK ? 0 : static_cast<int>(result);
        workerAttempted_ = true;
        lastWorkerAttempt_ = operations_.currentTime();
        if (result != osOK) {
            Serial.println("Local HTTP worker start failed; retrying later");
        }
    }
    stateMutex_.unlock();
}

LocalWebServerState LocalWebServer::state() const {
    stateMutex_.lock();
    LocalWebServerState snapshot = state_;
    stateMutex_.unlock();
    return snapshot;
}

LocalWebServer::RequestedState LocalWebServer::requestedState() const {
    stateMutex_.lock();
    RequestedState snapshot = requested_;
    stateMutex_.unlock();
    return snapshot;
}

bool LocalWebServer::isCurrent(uint32_t generation) const {
    RequestedState snapshot = requestedState();
    return !snapshot.shutdown && snapshot.address != 0 &&
        snapshot.generation == generation;
}

bool LocalWebServer::publishState(uint32_t generation, uint32_t address, int error) {
    stateMutex_.lock();
    bool current = !requested_.shutdown && requested_.generation == generation;
    if (current) {
        state_.listening = address != 0;
        state_.address = address;
        state_.error = error;
    }
    stateMutex_.unlock();
    return current;
}

void LocalWebServer::notifyService(bool available, uint32_t address) {
    if (serviceUpdate_) {
        serviceUpdate_(available, address);
    }
}

void LocalWebServer::run() {
    int listener = -1;
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
            if (listener >= 0) {
                operations_.closeSocket(listener);
                listener = -1;
            }
            generation = requested.generation;
            attempted = false;
        }

        if (requested.address != 0 && listener < 0 &&
            (!attempted || operations_.currentTime() - lastAttempt >=
                startRetryIntervalMs_)) {
            listener = operations_.openListener(requested.address, port_);
            attempted = true;
            lastAttempt = operations_.currentTime();
            if (!publishState(generation, listener >= 0 ? requested.address : 0,
                              listener >= 0 ? 0 : listener)) {
                if (listener >= 0) {
                    operations_.closeSocket(listener);
                    listener = -1;
                }
                continue;
            }
            if (listener < 0) {
                Serial.println("Local HTTP listener start failed; retrying later");
            }
        }

        if (listener >= 0 && isCurrent(generation)) {
            notifyService(true, requested.address);
            int client = operations_.acceptClient(listener);
            if (client >= 0) {
                serveClient(client, generation);
                operations_.closeSocket(client);
            } else if (client == ACCEPT_ERROR) {
                notifyService(false, 0);
                operations_.closeSocket(listener);
                listener = -1;
                publishState(generation, 0, ACCEPT_ERROR);
                lastAttempt = operations_.currentTime();
            }
        } else {
            notifyService(false, 0);
        }
        rtos::Thread::wait(20);
    }

    notifyService(false, 0);
    if (listener >= 0) {
        operations_.closeSocket(listener);
    }
}

void LocalWebServer::serveClient(int client, uint32_t generation) {
    char requestLine[REQUEST_LINE_SIZE];
    char body[RESPONSE_BODY_SIZE] = {};
    LocalHttpResponse response;
    if (!readRequest(client, requestLine, sizeof(requestLine), generation)) {
        strcpy(body, "{\"error\":\"bad request\"}");
        response = {"400 Bad Request", "application/json", strlen(body)};
    } else {
        response = handler_.handle(requestLine, body, sizeof(body));
        if (response.bodyLength > sizeof(body) || response.status == NULL ||
            response.contentType == NULL) {
            strcpy(body, "{\"error\":\"invalid response\"}");
            response = {"500 Internal Server Error", "application/json", strlen(body)};
        }
    }
    bool sent = sendResponse(client, response, body, generation);
    Serial.print("Local HTTP response: ");
    Serial.println(sent ? response.status : "send failed or session changed");
}

bool LocalWebServer::readRequest(
    int client,
    char *requestLine,
    size_t requestLineSize,
    uint32_t generation) {
    uint32_t requestStart = operations_.currentTime();
    size_t requestLength = 0;
    size_t headerBytes = 0;
    bool readingRequestLine = true;
    bool currentLineIsBlank = true;

    requestLine[0] = '\0';

    while (isCurrent(generation) &&
           operations_.currentTime() - requestStart < IO_TIMEOUT_MS) {
        char buffer[128];
        int received = operations_.receiveBytes(client, buffer, sizeof(buffer));
        if (received < 0 || received > static_cast<int>(sizeof(buffer))) {
            return false;
        }

        for (int offset = 0; offset < received; ++offset) {
            char current = buffer[offset];
            if (++headerBytes > MAX_HEADER_BYTES || current == '\0') {
                return false;
            }
            if (readingRequestLine) {
                if (current == '\n') {
                    requestLine[requestLength] = '\0';
                    readingRequestLine = false;
                } else if (current != '\r') {
                    if (requestLength + 1 >= requestLineSize) {
                        return false;
                    }
                    requestLine[requestLength++] = current;
                }
            }

            if (current == '\n' && currentLineIsBlank) {
                return requestLength > 0;
            }

            if (current == '\n') {
                currentLineIsBlank = true;
            } else if (current != '\r') {
                currentLineIsBlank = false;
            }
        }
        rtos::Thread::wait(1);
    }

    return false;
}

bool LocalWebServer::sendResponse(
    int client,
    const LocalHttpResponse &response,
    const char *body,
    uint32_t generation) {
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
    uint32_t started = operations_.currentTime();
    return sendAll(client, header, static_cast<size_t>(length), generation, started) &&
        sendAll(client, body, response.bodyLength, generation, started);
}

bool LocalWebServer::sendAll(
    int client, const char *buffer, size_t size,
    uint32_t generation, uint32_t started) {
    size_t sent = 0;
    while (sent < size && isCurrent(generation) &&
           operations_.currentTime() - started < IO_TIMEOUT_MS) {
        int count = operations_.sendBytes(client, buffer + sent, size - sent);
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