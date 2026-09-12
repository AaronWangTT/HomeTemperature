#ifndef LOCAL_WEB_SERVER_H
#define LOCAL_WEB_SERVER_H

#include <stdint.h>
#include "rtos.h"
#include "LocalHttpHandler.h"

struct LocalWebServerOperations {
    uint32_t (*currentTime)();
    int (*openListener)(uint32_t address, uint16_t port);
    int (*acceptClient)(int listener);
    int (*receiveBytes)(int client, char *buffer, size_t size);
    int (*sendBytes)(int client, const char *buffer, size_t size);
    void (*closeSocket)(int descriptor);
};

struct LocalWebServerState {
    bool workerStarted;
    bool listening;
    uint32_t address;
    int error;
};

typedef void (*LocalHttpServiceUpdate)(bool, uint32_t, void *);

class LocalWebServer {
public:
    static const int ACCEPT_IDLE = -1;
    static const int ACCEPT_ERROR = -2;

    LocalWebServer(
        LocalHttpHandler &handler,
        uint16_t port,
        uint32_t startRetryIntervalMs,
        LocalHttpServiceUpdate serviceUpdate = NULL,
        void *serviceContext = NULL);

    LocalWebServer(
        LocalHttpHandler &handler,
        uint16_t port,
        uint32_t startRetryIntervalMs,
        const LocalWebServerOperations &operations,
        LocalHttpServiceUpdate serviceUpdate = NULL,
        void *serviceContext = NULL);

    ~LocalWebServer();

    void update(bool wifiConnected, uint32_t address);
    LocalWebServerState state() const;

private:
    struct RequestedState {
        uint32_t address;
        uint32_t generation;
        bool shutdown;
    };

    LocalWebServer(const LocalWebServer &) = delete;
    LocalWebServer &operator=(const LocalWebServer &) = delete;
    static LocalWebServerOperations defaultOperations();
    RequestedState requestedState() const;
    bool isCurrent(uint32_t generation) const;
    bool publishState(uint32_t generation, uint32_t address, int error);
    void notifyService(bool available, uint32_t address);
    void run();
    void serveClient(int client, uint32_t generation);
    bool readRequest(
        int client,
        char *requestLine,
        size_t requestLineSize,
        uint32_t generation);
    bool sendResponse(
        int client,
        const LocalHttpResponse &response,
        const char *body,
        uint32_t generation);
    bool sendAll(int client, const char *buffer, size_t size,
                 uint32_t generation, uint32_t started);

    LocalHttpHandler &handler_;
    uint16_t port_;
    uint32_t startRetryIntervalMs_;
    LocalWebServerOperations operations_;
    LocalHttpServiceUpdate serviceUpdate_;
    void *serviceContext_;
    mutable rtos::Mutex stateMutex_;
    rtos::Thread worker_;
    RequestedState requested_;
    LocalWebServerState state_;
    uint32_t lastWorkerAttempt_;
    bool workerAttempted_;
};

#endif