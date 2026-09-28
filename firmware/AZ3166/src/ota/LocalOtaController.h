#ifndef LOCAL_OTA_CONTROLLER_H
#define LOCAL_OTA_CONTROLLER_H

#include <stddef.h>
#include <stdint.h>
#include "rtos.h"
#include "../http/LocalHttpStreamingHandler.h"
#include "LocalOtaPlatform.h"
#include "NetworkMaintenanceCoordinator.h"

enum LocalOtaState {
    LOCAL_OTA_DISABLED,
    LOCAL_OTA_IDLE,
    LOCAL_OTA_CHALLENGE_DISPLAYED,
    LOCAL_OTA_ARMED,
    LOCAL_OTA_WAITING_FOR_NETWORK_LEASE,
    LOCAL_OTA_RECEIVING,
    LOCAL_OTA_VERIFYING,
    LOCAL_OTA_READY,
    LOCAL_OTA_APPLYING,
    LOCAL_OTA_ACTIVATED_AWAITING_RESPONSE,
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
typedef void (*LocalOtaUploadHandoffHook)(bool afterFinish, void *context);

class LocalOtaController {
public:
    static const uint32_t CHALLENGE_WINDOW_MS = 30000UL;
    static const uint32_t CAPABILITY_WINDOW_MS = 300000UL;
    static const uint32_t NETWORK_LEASE_WAIT_MS = 10000UL;
    static const uint32_t APPLY_WAIT_MS =
        OTA_STAGING_ACTIVATION_MAX_MS + 2000UL;

    LocalOtaController(
        LocalOtaCore &core,
        LocalOtaEntropy &entropy,
        LocalOtaDisplay &display,
        NetworkMaintenanceCoordinator &network,
        const uint8_t *publicKey,
        size_t publicKeySize,
        const char *productId,
        const char *boardId,
        const char *currentVersion,
        LocalOtaClock clock,
        LocalOtaBeforeApplyValidation beforeApplyValidation = NULL,
        void *applyValidationContext = NULL,
        LocalOtaUploadHandoffHook uploadHandoffHook = NULL,
        void *uploadHandoffContext = NULL);
    ~LocalOtaController();

    bool begin();
    void shutdown();
    bool openChallenge();
    bool claim(
        const char *challenge,
        uint32_t peerAddress,
        uint32_t networkGeneration,
        char capabilityHex[33]);
    bool authorize(const LocalHttpRequest &request, uint32_t &generation);
    bool upload(
        const LocalHttpStreamingRequest &request,
        LocalHttpBodyStream &body);
    bool apply(const LocalHttpRequest &request, OTAStagingError &result);
    bool cancel(const LocalHttpRequest &request);
    void update(uint32_t networkGeneration);
    LocalOtaSnapshot snapshot() const;
    const char *stateName(LocalOtaState state) const;
    const char *errorName(OTAStagingError error);
    void responseAttempted(bool sent);
    bool takeRebootRequest();

private:
    enum WorkerCommand {
        WORKER_NONE,
        WORKER_UPLOAD,
        WORKER_APPLY,
        WORKER_CANCEL,
        WORKER_SHUTDOWN
    };

    static int admit(const OTAStagingMetadata *metadata, void *context);
    static int cancelled(void *context);
    static void runWorkerThunk(LocalOtaController *controller);
    void runWorker();
    void processUpload();
    void processApply();
    bool metadataAllowed(const OTAStagingMetadata &metadata) const;
    bool capabilityValidLocked(
        const LocalHttpRequest &request,
        uint32_t now,
        uint32_t &generation) const;
    bool queuedApplyValidLocked(uint32_t now) const;
    bool uploadCanAdvanceLocked(
        uint32_t expectedGeneration,
        LocalOtaState expectedState) const;
    bool requestCancellationLocked(uint32_t expectedGeneration);
    void fail(OTAStagingError error, bool fatal);
    void releaseLease(uint32_t generation);
    void clearCapabilityLocked();
    static bool parseVersion(
        const char *version, uint16_t &major, uint16_t &minor, uint16_t &patch);

    LocalOtaCore &core_;
    LocalOtaEntropy &entropy_;
    LocalOtaDisplay &display_;
    NetworkMaintenanceCoordinator &network_;
    const uint8_t *publicKey_;
    size_t publicKeySize_;
    const char *productId_;
    const char *boardId_;
    const char *currentVersion_;
    LocalOtaClock clock_;
    LocalOtaBeforeApplyValidation beforeApplyValidation_;
    void *applyValidationContext_;
    LocalOtaUploadHandoffHook uploadHandoffHook_;
    void *uploadHandoffContext_;
    mutable rtos::Mutex mutex_;
    rtos::Thread worker_;
    rtos::Semaphore commandSignal_;
    rtos::Semaphore uploadCompletion_;
    rtos::Semaphore applyCompletion_;
    bool workerStarted_;
    bool shutdown_;
    WorkerCommand command_;
    LocalOtaState state_;
    uint32_t generation_;
    uint32_t networkGeneration_;
    uint32_t peerAddress_;
    uint32_t challengeDeadline_;
    uint32_t capabilityDeadline_;
    uint32_t leaseDeadline_;
    uint32_t reservedGeneration_;
    uint8_t challenge_[4];
    uint8_t capability_[16];
    uint8_t failedClaims_;
    bool cancelRequested_;
    bool uploadCompleted_;
    bool applyCompleted_;
    bool rebootPending_;
    bool activationResponseAttempted_;
    uint32_t activationSuccessGeneration_;
    LocalHttpBodyStream *uploadBody_;
    size_t packageSize_;
    size_t acceptedBytes_;
    OTAStagedImageInfo staged_;
    OTAStagingError lastError_;
    OTAStagingError applyResult_;
    struct {
        bool pending;
        uint8_t capability[16];
        uint32_t peerAddress;
        uint32_t controllerGeneration;
        uint32_t networkGeneration;
        uint32_t deadline;
    } queuedApply_;
};

#endif
