#ifndef LOCAL_OTA_CONTROLLER_H
#define LOCAL_OTA_CONTROLLER_H

#include <stddef.h>
#include <stdint.h>
#include "rtos.h"
#include "../http/LocalHttpStreamingHandler.h"
#include "LocalOtaPlatform.h"
#include "../connectivity/NetworkMaintenanceCoordinator.h"

enum LocalOtaState {
    LOCAL_OTA_DISABLED,
    LOCAL_OTA_IDLE,
    LOCAL_OTA_WAITING_FOR_NETWORK_LEASE,
    LOCAL_OTA_RECEIVING,
    LOCAL_OTA_VERIFYING,
    LOCAL_OTA_READY,
    LOCAL_OTA_APPLYING,
    LOCAL_OTA_ERROR,
    LOCAL_OTA_FATAL
};

struct LocalOtaSnapshot {
    LocalOtaState state;
    uint32_t generation;
    size_t acceptedBytes;
    size_t totalBytes;
    OTAStagingError lastError;
    bool rebootPending;
};

typedef uint32_t (*LocalOtaClock)();
typedef void (*LocalOtaBeforeApplyValidation)(void *context);

class LocalOtaController {
public:
    static const uint32_t NETWORK_LEASE_WAIT_MS = 10000UL;

    LocalOtaController(
        LocalOtaCore &core,
        NetworkMaintenanceCoordinator &network,
        const uint8_t *publicKey,
        size_t publicKeySize,
        const char *productId,
        const char *boardId,
        const char *currentVersion,
        LocalOtaClock clock,
        LocalOtaBeforeApplyValidation beforeApplyValidation = NULL,
        void *applyValidationContext = NULL);
    ~LocalOtaController();

    bool begin();
    bool upload(
        const LocalHttpStreamingRequest &request,
        LocalHttpBodyStream &body);
    bool apply(
        uint32_t expectedGeneration,
        const uint8_t expectedDigest[OTA_SHA256_SIZE],
        OTAStagingError &result,
        void *&responseContext);
    bool cancel();
    void update(uint32_t networkGeneration);
    LocalOtaSnapshot snapshot() const;
    const char *stateName(LocalOtaState state) const;
    const char *errorName(OTAStagingError error);
    bool readyImage(
        uint32_t &generation,
        uint8_t digest[OTA_SHA256_SIZE]) const;
    static void responseAttempted(bool sent, void *context);
    bool takeRebootRequest();

private:
    enum WorkerCommand {
        WORKER_NONE,
        WORKER_UPLOAD,
        WORKER_APPLY,
        WORKER_CANCEL,
        WORKER_SHUTDOWN
    };

    struct ApplyResponseContext {
        LocalOtaController *controller;
        uint32_t controllerGeneration;
        bool active;
    };

    static int admit(const OTAStagingMetadata *metadata, void *context);
    static int cancelled(void *context);
    static void runWorkerThunk(LocalOtaController *controller);
    void runWorker();
    void processUpload();
    void processApply();
    bool metadataAllowed(const OTAStagingMetadata &metadata) const;
    bool queuedApplyValidLocked() const;
    bool requestCancellationLocked(uint32_t expectedGeneration);
    void fail(OTAStagingError error, bool fatal);
    void releaseLease(uint32_t generation);
    static bool parseVersion(
        const char *version, uint16_t &major, uint16_t &minor, uint16_t &patch);

    LocalOtaCore &core_;
    NetworkMaintenanceCoordinator &network_;
    const uint8_t *publicKey_;
    size_t publicKeySize_;
    const char *productId_;
    const char *boardId_;
    const char *currentVersion_;
    LocalOtaClock clock_;
    LocalOtaBeforeApplyValidation beforeApplyValidation_;
    void *applyValidationContext_;
    mutable rtos::Mutex mutex_;
    rtos::Thread worker_;
    rtos::Semaphore commandSignal_;
    rtos::Semaphore uploadCompletion_;
    bool workerStarted_;
    bool shutdown_;
    WorkerCommand command_;
    LocalOtaState state_;
    uint32_t generation_;
    uint32_t networkGeneration_;
    uint32_t leaseDeadline_;
    bool cancelRequested_;
    bool uploadCompleted_;
    bool rebootPending_;
    LocalHttpBodyStream *uploadBody_;
    size_t packageSize_;
    size_t acceptedBytes_;
    OTAStagedImageInfo staged_;
    OTAStagingError lastError_;
    struct {
        bool pending;
        bool responseReleased;
        uint32_t controllerGeneration;
    } queuedApply_;
    ApplyResponseContext applyResponseContexts_[2];
};

#endif
