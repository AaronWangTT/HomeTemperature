#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

class DeviceIdentity {
public:
    static const size_t DEVICE_ID_SIZE = 32;

    DeviceIdentity();

    bool begin();
    bool isInitialized() const;
    const char *get() const;

    static bool format(
        char *output,
        size_t outputSize,
        uint32_t uid0,
        uint32_t uid1,
        uint32_t uid2);

private:
    char deviceId_[DEVICE_ID_SIZE];
    bool initialized_;
};

#endif