#ifndef AZ3166_LOCAL_WEB_SERVER_OPERATIONS_H
#define AZ3166_LOCAL_WEB_SERVER_OPERATIONS_H

#include "LocalWebServer.h"
#include "lwip/sockets.h"

class Az3166LocalWebServerOperations : public LocalWebServerOperations {
public:
    uint32_t currentTime() override;
    int openListener(uint32_t address, uint16_t port) override;
    int acceptClient(int listener) override;
    int receiveBytes(int client, char *buffer, size_t size) override;
    int sendBytes(int client, const char *buffer, size_t size) override;
    bool peerIpv4(int client, uint32_t &address) override;
    void closeSocket(int descriptor) override;
    bool supportsConcurrentSockets() const override { return true; }

protected:
    virtual int listenerReady(int listener);
    virtual int acceptSocket(int listener);
    virtual int getSocketOption(int descriptor, int level, int option,
                                void *value, socklen_t *length);
    virtual int getPeerName(int descriptor, sockaddr *address,
                            socklen_t *length);
};

#endif