#include "LocalOtaPlatform.h"

#include <Arduino.h>
#include "hal/trng_api.h"

bool Az3166OtaEntropy::fill(uint8_t *output, size_t size) {
    if (output == NULL || size == 0) {
        return false;
    }
    trng_t trng;
    trng_init(&trng);
    size_t produced = 0;
    int result = trng_get_bytes(&trng, output, size, &produced);
    trng_free(&trng);
    return result == 0 && produced == size;
}

void Az3166OtaDisplay::showChallenge(const char *challenge) {
    Screen.print(0, "Local OTA");
    Screen.print(1, challenge == NULL ? "Unavailable" : challenge);
    Screen.print(2, "Expires in 30 sec");
}

void Az3166OtaDisplay::clearChallenge() {
    Screen.print(0, " ");
    Screen.print(1, " ");
    Screen.print(2, " ");
}

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
