#ifndef DEVICE_HOMEPAGE_HANDLER_H
#define DEVICE_HOMEPAGE_HANDLER_H

#include <stddef.h>
#include <stdint.h>
#include "rtos.h"
#include "LocalHttpHandler.h"

class DeviceHomepageHandler : public LocalHttpHandler {
public:
    DeviceHomepageHandler(
        LocalHttpHandler &fallback,
        const char *hostname,
        const char *location,
        const char *region,
        const char *firmwareVersion,
        uint16_t httpPort);

    bool begin(const char *deviceId);
    void updateNetwork(
        bool connected,
        uint32_t address,
        const uint8_t macAddress[6]);

    LocalHttpResponse handle(
        const char *requestLine,
        char *body,
        size_t bodySize) override;

private:
    static bool exactGet(const char *requestLine, const char *path);
    LocalHttpResponse deviceInfo(char *body, size_t bodySize);

    LocalHttpHandler &fallback_;
    const char *hostname_;
    const char *location_;
    const char *region_;
    const char *firmwareVersion_;
    uint16_t httpPort_;
    rtos::Mutex stateMutex_;
    char deviceId_[32];
    char macAddress_[18];
    uint32_t address_;
    bool connected_;
};

#endif
