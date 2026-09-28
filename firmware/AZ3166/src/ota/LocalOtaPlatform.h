#ifndef LOCAL_OTA_PLATFORM_H
#define LOCAL_OTA_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <OTAStaging.h>

class LocalOtaCore {
public:
    virtual ~LocalOtaCore() {}
    virtual OTAStagingError begin(
        size_t packageSize,
        const uint8_t *key,
        size_t keySize,
        OTAAdmissionCallback admission,
        OTACancellationCallback cancellation,
        void *context) = 0;
    virtual OTAStagingError write(const uint8_t *data, size_t size) = 0;
    virtual OTAStagingError finish(OTAStagedImageInfo *info) = 0;
    virtual OTAStagingError activate(
        uint32_t generation,
        const uint8_t digest[OTA_SHA256_SIZE]) = 0;
    virtual OTAStagingError abort() = 0;
    virtual OTAStagingStatus status() = 0;
    virtual const char *errorName(OTAStagingError error) = 0;
};

class Az3166OtaCore : public LocalOtaCore {
public:
    OTAStagingError begin(
        size_t packageSize,
        const uint8_t *key,
        size_t keySize,
        OTAAdmissionCallback admission,
        OTACancellationCallback cancellation,
        void *context) override;
    OTAStagingError write(const uint8_t *data, size_t size) override;
    OTAStagingError finish(OTAStagedImageInfo *info) override;
    OTAStagingError activate(
        uint32_t generation,
        const uint8_t digest[OTA_SHA256_SIZE]) override;
    OTAStagingError abort() override;
    OTAStagingStatus status() override;
    const char *errorName(OTAStagingError error) override;
};

#endif
