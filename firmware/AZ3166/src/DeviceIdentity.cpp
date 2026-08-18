#include "DeviceIdentity.h"

#include <Arduino.h>
#include <stdio.h>

namespace {

const uintptr_t STM32_UID_ADDRESS = 0x1FFF7A10UL;
const char UNINITIALIZED_DEVICE_ID[] =
    "az3166-FFFFFFFFFFFFFFFFFFFFFFFF";

static_assert(
    sizeof(UNINITIALIZED_DEVICE_ID) == DeviceIdentity::DEVICE_ID_SIZE,
    "Fallback device ID must match the generated device ID size");

}  // namespace

DeviceIdentity::DeviceIdentity()
    : initialized_(false) {
    deviceId_[0] = '\0';
}

bool DeviceIdentity::format(
    char *output,
    size_t outputSize,
    uint32_t uid0,
    uint32_t uid1,
    uint32_t uid2) {
    if (output == NULL || outputSize == 0) {
        return false;
    }

    int length = snprintf(
        output,
        outputSize,
        "az3166-%08lX%08lX%08lX",
        static_cast<unsigned long>(uid0),
        static_cast<unsigned long>(uid1),
        static_cast<unsigned long>(uid2));
    if (length < 0 || static_cast<size_t>(length) >= outputSize) {
        output[0] = '\0';
        return false;
    }

    return true;
}

bool DeviceIdentity::begin() {
    volatile const uint32_t *uid =
        reinterpret_cast<volatile const uint32_t *>(STM32_UID_ADDRESS);
    initialized_ = format(
        deviceId_,
        sizeof(deviceId_),
        uid[0],
        uid[1],
        uid[2]);
    if (initialized_) {
        Serial.print("Device ID: ");
        Serial.println(deviceId_);
    } else {
        Serial.println("Device ID initialization failed");
    }
    return initialized_;
}

bool DeviceIdentity::isInitialized() const {
    return initialized_;
}

const char *DeviceIdentity::get() const {
    return initialized_
        ? deviceId_
        : UNINITIALIZED_DEVICE_ID;
}