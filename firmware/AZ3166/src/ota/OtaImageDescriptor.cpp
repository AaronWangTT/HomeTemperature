#include <stddef.h>
#include <stdint.h>

#include "../config/AppConfig.h"

#ifndef HOME_TEMPERATURE_OTA_SOURCE_COMMIT_BYTES
#define HOME_TEMPERATURE_OTA_SOURCE_COMMIT_BYTES \
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, \
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, \
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, \
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, \
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30
#endif

#ifndef HOME_TEMPERATURE_OTA_KEY_ID_BYTES
#define HOME_TEMPERATURE_OTA_KEY_ID_BYTES \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#endif

namespace {

struct __attribute__((packed)) OtaImageDescriptor {
    char magic[8];
    uint16_t descriptorVersion;
    uint16_t descriptorSize;
    uint32_t securityProfile;
    char productId[32];
    char boardId[32];
    char firmwareVersion[32];
    uint8_t sourceCommit[40];
    uint32_t applicationAddress;
    uint32_t applicationCapacity;
    uint32_t packageFormatVersion;
    uint8_t keyId[32];
    uint8_t reserved[60];
};

static_assert(sizeof(OtaImageDescriptor) == 256, "OTA descriptor size changed");
static_assert(
    offsetof(OtaImageDescriptor, keyId) == 164,
    "OTA descriptor key identifier offset changed");

}

extern "C" const OtaImageDescriptor homeTemperatureOtaImageDescriptor
    __attribute__((section(".ota_descriptor"), used, aligned(4))) = {
        {'A', 'Z', 'O', 'T', 'A', '0', '0', '1'},
        1,
        sizeof(OtaImageDescriptor),
        1,
        "HomeTemperature",
        "MXCHIP_AZ3166",
        HOME_TEMPERATURE_FIRMWARE_VERSION,
        {HOME_TEMPERATURE_OTA_SOURCE_COMMIT_BYTES},
        0x0800C000,
        0x000F4000,
        1,
        {HOME_TEMPERATURE_OTA_KEY_ID_BYTES},
        {0}
    };
