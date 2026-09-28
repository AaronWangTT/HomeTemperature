#include "LocalOtaPlatform.h"

#include <Arduino.h>

OTAStagingError Az3166OtaCore::begin(
    size_t packageSize,
    const uint8_t *key,
    size_t keySize,
    OTAAdmissionCallback admission,
    OTACancellationCallback cancellation,
    void *context) {
    return OTAStagingBegin(
        packageSize, key, keySize, admission, cancellation, context);
}

OTAStagingError Az3166OtaCore::write(const uint8_t *data, size_t size) {
    return OTAStagingWritePackage(data, size);
}

OTAStagingError Az3166OtaCore::finish(OTAStagedImageInfo *info) {
    return OTAStagingFinish(info);
}

OTAStagingError Az3166OtaCore::activate(
    uint32_t generation,
    const uint8_t digest[OTA_SHA256_SIZE]) {
    return OTAStagingActivate(generation, digest);
}

OTAStagingError Az3166OtaCore::abort() {
    return OTAStagingAbort();
}

OTAStagingStatus Az3166OtaCore::status() {
    return OTAStagingGetStatus();
}

const char *Az3166OtaCore::errorName(OTAStagingError error) {
    return OTAStagingErrorName(error);
}
