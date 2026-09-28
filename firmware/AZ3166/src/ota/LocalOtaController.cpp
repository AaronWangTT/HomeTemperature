#include "LocalOtaController.h"

#include <mutex>
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

namespace {

uint32_t defaultClock() {
    return millis();
}

bool elapsed(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

}

LocalOtaController::LocalOtaController(
    LocalOtaCore &core,
    NetworkMaintenanceCoordinator &network,
    const uint8_t *publicKey,
    size_t publicKeySize,
    const char *productId,
    const char *boardId,
    const char *currentVersion,
    LocalOtaClock clock,
    LocalOtaBeforeApplyValidation beforeApplyValidation,
    void *applyValidationContext)
    : core_(core),
      network_(network),
      publicKey_(publicKey),
      publicKeySize_(publicKeySize),
      productId_(productId),
      boardId_(boardId),
      currentVersion_(currentVersion),
      clock_(clock == NULL ? defaultClock : clock),
      beforeApplyValidation_(beforeApplyValidation),
      applyValidationContext_(applyValidationContext),
      worker_(osPriorityNormal, 8192),
      commandSignal_(0),
      uploadCompletion_(0),
      applyCompletion_(0),
      workerStarted_(false),
      shutdown_(false),
      command_(WORKER_NONE),
      state_(publicKey == NULL || publicKeySize == 0 ||
                 currentVersion == NULL || currentVersion[0] == '\0'
                 ? LOCAL_OTA_DISABLED : LOCAL_OTA_IDLE),
      generation_(0),
      networkGeneration_(0),
      leaseDeadline_(0),
      cancelRequested_(false),
      uploadCompleted_(false),
      applyCompleted_(false),
      rebootPending_(false),
      uploadBody_(NULL),
      packageSize_(0),
      acceptedBytes_(0),
      staged_{},
      lastError_(OTA_OK),
      applyResult_(OTA_ERROR_INVALID_STATE),
      queuedApply_{} {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
    if (state_ != LOCAL_OTA_DISABLED &&
        !parseVersion(currentVersion_, major, minor, patch)) {
        state_ = LOCAL_OTA_DISABLED;
    }
}

LocalOtaController::~LocalOtaController() {
    if (!workerStarted_) {
        return;
    }
    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        shutdown_ = true;
        cancelRequested_ = true;
        command_ = WORKER_SHUTDOWN;
    }
    commandSignal_.release();
    worker_.join();
}

bool LocalOtaController::begin() {
    if (state_ == LOCAL_OTA_DISABLED) {
        return false;
    }
    osStatus result = worker_.start(
        mbed::callback(LocalOtaController::runWorkerThunk, this));
    workerStarted_ = result == osOK;
    if (!workerStarted_) {
        state_ = LOCAL_OTA_DISABLED;
    }
    return workerStarted_;
}

bool LocalOtaController::upload(
    const LocalHttpStreamingRequest &request,
    LocalHttpBodyStream &body) {
    uint32_t uploadGeneration;
    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        if ((state_ != LOCAL_OTA_IDLE && state_ != LOCAL_OTA_ERROR) ||
            request.contentLength < OTA_PACKAGE_PAYLOAD_OFFSET + 1 ||
            command_ != WORKER_NONE) {
            return false;
        }
        ++generation_;
        if (generation_ == 0) {
            ++generation_;
        }
        uploadGeneration = generation_;
        networkGeneration_ = request.metadata.networkGeneration;
        if (!network_.reserveOta(uploadGeneration)) {
            return false;
        }
        state_ = LOCAL_OTA_WAITING_FOR_NETWORK_LEASE;
        packageSize_ = request.contentLength;
        acceptedBytes_ = 0;
        uploadBody_ = &body;
        uploadCompleted_ = false;
        leaseDeadline_ = clock_() + NETWORK_LEASE_WAIT_MS;
        cancelRequested_ = false;
        command_ = WORKER_UPLOAD;
    }
    commandSignal_.release();
    while (true) {
        uploadCompletion_.wait(50);
        bool wake = false;
        {
            std::lock_guard<rtos::Mutex> lock(mutex_);
            if (uploadCompleted_) {
                return state_ == LOCAL_OTA_READY;
            }
            if (body.cancelled()) {
                wake = requestCancellationLocked(uploadGeneration);
            }
        }
        if (wake) {
            commandSignal_.release();
        }
    }
}

bool LocalOtaController::apply(
    uint32_t expectedGeneration,
    const uint8_t expectedDigest[OTA_SHA256_SIZE],
    OTAStagingError &result) {
    uint32_t generation;
    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        generation = generation_;
        if (expectedDigest == NULL ||
            state_ != LOCAL_OTA_READY ||
            command_ != WORKER_NONE ||
            staged_.sessionGeneration != expectedGeneration ||
            memcmp(
                staged_.sha256, expectedDigest, OTA_SHA256_SIZE) != 0) {
            return false;
        }
        applyCompleted_ = false;
        applyResult_ = OTA_ERROR_INVALID_STATE;
        queuedApply_.pending = true;
        queuedApply_.controllerGeneration = generation;
        command_ = WORKER_APPLY;
        result = OTA_OK;
    }
    return true;
}

bool LocalOtaController::cancel() {
    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        if (state_ == LOCAL_OTA_IDLE || state_ == LOCAL_OTA_DISABLED ||
            state_ == LOCAL_OTA_APPLYING || state_ == LOCAL_OTA_FATAL) {
            return false;
        }
        requestCancellationLocked(generation_);
    }
    commandSignal_.release();
    return true;
}

void LocalOtaController::update(uint32_t networkGeneration) {
    uint32_t now = clock_();
    bool wake = false;
    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        if ((state_ == LOCAL_OTA_WAITING_FOR_NETWORK_LEASE ||
                    state_ == LOCAL_OTA_RECEIVING ||
                    state_ == LOCAL_OTA_VERIFYING ||
                    state_ == LOCAL_OTA_READY) &&
                   (networkGeneration != networkGeneration_ ||
                    (state_ == LOCAL_OTA_WAITING_FOR_NETWORK_LEASE &&
                     elapsed(now, leaseDeadline_)))) {
            wake = requestCancellationLocked(generation_);
        } else if (state_ == LOCAL_OTA_WAITING_FOR_NETWORK_LEASE &&
                   network_.tryGrantOta(generation_)) {
            state_ = LOCAL_OTA_RECEIVING;
            wake = true;
        }
    }
    if (wake) {
        commandSignal_.release();
    }
}

LocalOtaSnapshot LocalOtaController::snapshot() const {
    std::lock_guard<rtos::Mutex> lock(mutex_);
    LocalOtaSnapshot result = {
        state_, generation_, acceptedBytes_, packageSize_, lastError_,
        rebootPending_
    };
    return result;
}

const char *LocalOtaController::stateName(LocalOtaState state) const {
    static const char *NAMES[] = {
        "Disabled", "Idle", "WaitingForNetworkLease", "Receiving", "Verifying", "Ready",
        "Applying", "Error", "Fatal"
    };
    return state >= LOCAL_OTA_DISABLED && state <= LOCAL_OTA_FATAL
        ? NAMES[state] : "Unknown";
}

const char *LocalOtaController::errorName(OTAStagingError error) {
    return core_.errorName(error);
}

bool LocalOtaController::readyImage(
    uint32_t &generation,
    uint8_t digest[OTA_SHA256_SIZE]) const {
    if (digest == NULL) {
        return false;
    }
    std::lock_guard<rtos::Mutex> lock(mutex_);
    if (state_ != LOCAL_OTA_READY) {
        return false;
    }
    generation = staged_.sessionGeneration;
    memcpy(digest, staged_.sha256, OTA_SHA256_SIZE);
    return true;
}

void LocalOtaController::responseAttempted(bool) {
    bool startApply = false;
    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        if (queuedApply_.pending && command_ == WORKER_APPLY) {
            startApply = true;
        }
    }
    if (startApply) {
        commandSignal_.release();
    }
}

bool LocalOtaController::takeRebootRequest() {
    std::lock_guard<rtos::Mutex> lock(mutex_);
    bool pending = rebootPending_;
    rebootPending_ = false;
    return pending;
}

int LocalOtaController::admit(
    const OTAStagingMetadata *metadata, void *context) {
    // Core accepts a candidate only when the admission callback returns nonzero.
    return metadata != NULL &&
        static_cast<LocalOtaController *>(context)->metadataAllowed(*metadata)
        ? 1 : 0;
}

int LocalOtaController::cancelled(void *context) {
    LocalOtaController *controller =
        static_cast<LocalOtaController *>(context);
    std::lock_guard<rtos::Mutex> lock(controller->mutex_);
    return controller->cancelRequested_ || controller->shutdown_ ? 1 : 0;
}

void LocalOtaController::runWorkerThunk(LocalOtaController *controller) {
    controller->runWorker();
}

void LocalOtaController::runWorker() {
    for (;;) {
        commandSignal_.wait();
        WorkerCommand command;
        bool abortForShutdown = false;
        bool shouldShutdown = false;
        {
            std::lock_guard<rtos::Mutex> lock(mutex_);
            command = command_;
            shouldShutdown = shutdown_;
            if (shouldShutdown) {
                abortForShutdown =
                    state_ == LOCAL_OTA_RECEIVING ||
                    state_ == LOCAL_OTA_VERIFYING ||
                    state_ == LOCAL_OTA_READY;
            }
        }
        if (shouldShutdown) {
            if (abortForShutdown) {
                core_.abort();
            }
            uint32_t shutdownGeneration;
            bool releaseUpload;
            bool releaseApply;
            {
                std::lock_guard<rtos::Mutex> lock(mutex_);
                shutdownGeneration = generation_;
                releaseUpload = uploadBody_ != NULL && !uploadCompleted_;
                releaseApply = queuedApply_.pending && !applyCompleted_;
                uploadBody_ = NULL;
                uploadCompleted_ = true;
                applyCompleted_ = true;
                queuedApply_.pending = false;
                command_ = WORKER_NONE;
                lastError_ = OTA_ERROR_CANCELLED;
                if (state_ != LOCAL_OTA_FATAL) {
                    state_ = LOCAL_OTA_IDLE;
                }
            }
            releaseLease(shutdownGeneration);
            if (releaseUpload) {
                uploadCompletion_.release();
            }
            if (releaseApply) {
                applyCompletion_.release();
            }
            break;
        }
        if (command == WORKER_UPLOAD) {
            bool ready;
            bool cancel;
            {
                std::lock_guard<rtos::Mutex> lock(mutex_);
                ready = state_ == LOCAL_OTA_RECEIVING;
                cancel = cancelRequested_;
                if (cancel) {
                    command_ = WORKER_CANCEL;
                }
            }
            if (ready && !cancel) {
                processUpload();
            } else if (cancel) {
                fail(OTA_ERROR_CANCELLED, false);
                {
                    std::lock_guard<rtos::Mutex> lock(mutex_);
                    uploadCompleted_ = true;
                }
                uploadCompletion_.release();
            }
        } else if (command == WORKER_APPLY) {
            processApply();
        } else if (command == WORKER_CANCEL) {
            LocalOtaState state;
            bool uploadPending;
            bool applyPending;
            {
                std::lock_guard<rtos::Mutex> lock(mutex_);
                state = state_;
                uploadPending = uploadBody_ != NULL && !uploadCompleted_;
                applyPending = queuedApply_.pending && !applyCompleted_;
            }
            if (state == LOCAL_OTA_READY || state == LOCAL_OTA_RECEIVING ||
                state == LOCAL_OTA_VERIFYING) {
                core_.abort();
            }
            fail(OTA_ERROR_CANCELLED, false);
            if (uploadPending) {
                {
                    std::lock_guard<rtos::Mutex> lock(mutex_);
                    uploadCompleted_ = true;
                }
                uploadCompletion_.release();
            }
            if (applyPending) {
                {
                    std::lock_guard<rtos::Mutex> lock(mutex_);
                    queuedApply_.pending = false;
                    applyResult_ = OTA_ERROR_CANCELLED;
                    applyCompleted_ = true;
                }
                applyCompletion_.release();
            }
        }
    }
}

void LocalOtaController::processUpload() {
    size_t packageSize;
    LocalHttpBodyStream *body;
    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        packageSize = packageSize_;
        body = uploadBody_;
    }
    OTAStagingError result = core_.begin(
        packageSize, publicKey_, publicKeySize_, admit, cancelled, this);
    uint8_t buffer[512];
    while (result == OTA_OK) {
        size_t received = 0;
        LocalHttpBodyReadStatus readStatus =
            body->read(reinterpret_cast<char *>(buffer), sizeof(buffer), received);
        if (readStatus == LOCAL_HTTP_BODY_COMPLETE) {
            break;
        }
        if (readStatus != LOCAL_HTTP_BODY_DATA) {
            result = (readStatus == LOCAL_HTTP_BODY_CANCELLED ||
                      readStatus == LOCAL_HTTP_BODY_DISCONNECTED)
                ? OTA_ERROR_CANCELLED : OTA_ERROR_INCOMPLETE;
            break;
        }
        if (received == 0) {
            if (cancelled(this)) {
                result = OTA_ERROR_CANCELLED;
                break;
            }
            continue;
        }
        if (cancelled(this)) {
            result = OTA_ERROR_CANCELLED;
            break;
        }
        result = core_.write(buffer, received);
        if (result == OTA_OK) {
            std::lock_guard<rtos::Mutex> lock(mutex_);
            acceptedBytes_ += received;
        }
    }
    if (result == OTA_OK && cancelled(this)) {
        result = OTA_ERROR_CANCELLED;
    }
    if (result == OTA_OK && body->remaining() == 0) {
        {
            std::lock_guard<rtos::Mutex> lock(mutex_);
            state_ = LOCAL_OTA_VERIFYING;
        }
        OTAStagedImageInfo staged = {};
        result = core_.finish(&staged);
        if (result == OTA_OK) {
            bool publishReady;
            {
                std::lock_guard<rtos::Mutex> lock(mutex_);
                publishReady = !cancelRequested_ &&
                    state_ == LOCAL_OTA_VERIFYING;
                if (publishReady) {
                    staged_ = staged;
                    state_ = LOCAL_OTA_READY;
                    lastError_ = OTA_OK;
                    command_ = WORKER_NONE;
                    uploadBody_ = NULL;
                    uploadCompleted_ = true;
                }
            }
            if (publishReady) {
                uploadCompletion_.release();
                return;
            }
            result = OTA_ERROR_CANCELLED;
        }
    }
    core_.abort();
    fail(result, false);
    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        uploadCompleted_ = true;
    }
    uploadCompletion_.release();
}

void LocalOtaController::processApply() {
    OTAStagedImageInfo staged;
    uint32_t generation;
    bool abortInstead = false;
    bool fatalAlreadyWon = false;
    if (beforeApplyValidation_ != NULL) {
        beforeApplyValidation_(applyValidationContext_);
    }
    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        generation = generation_;
        if (!queuedApplyValidLocked()) {
            fatalAlreadyWon = state_ == LOCAL_OTA_FATAL;
            command_ = WORKER_CANCEL;
            applyResult_ = OTA_ERROR_CANCELLED;
            applyCompleted_ = true;
            queuedApply_.pending = false;
            abortInstead = true;
        } else {
            state_ = LOCAL_OTA_APPLYING;
            queuedApply_.pending = false;
            staged = staged_;
        }
    }
    if (abortInstead) {
        core_.abort();
        if (!fatalAlreadyWon) {
            fail(OTA_ERROR_CANCELLED, false);
        }
        applyCompletion_.release();
        return;
    }
    OTAStagingError result =
        core_.activate(staged.sessionGeneration, staged.sha256);
    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        if (state_ == LOCAL_OTA_FATAL) {
            applyResult_ = OTA_ERROR_ACTIVATION_UNCERTAIN;
            applyCompleted_ = true;
            command_ = WORKER_NONE;
            applyCompletion_.release();
            return;
        }
        applyResult_ = result;
        lastError_ = result;
        applyCompleted_ = true;
        command_ = WORKER_NONE;
        if (result == OTA_OK) {
            state_ = LOCAL_OTA_IDLE;
            rebootPending_ = true;
        } else if (result == OTA_ERROR_ACTIVATION_UNCERTAIN) {
            state_ = LOCAL_OTA_FATAL;
        } else {
            state_ = LOCAL_OTA_ERROR;
        }
    }
    if (result != OTA_ERROR_ACTIVATION_UNCERTAIN) {
        releaseLease(generation);
    }
    applyCompletion_.release();
}

void LocalOtaController::fail(OTAStagingError error, bool fatal) {
    uint32_t generation;
    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        generation = generation_;
        lastError_ = error;
        command_ = WORKER_NONE;
        cancelRequested_ = false;
        uploadBody_ = NULL;
        queuedApply_.pending = false;
        if (fatal) {
            state_ = LOCAL_OTA_FATAL;
        } else if (error == OTA_ERROR_CANCELLED) {
            state_ = LOCAL_OTA_IDLE;
        } else {
            state_ = LOCAL_OTA_ERROR;
        }
    }
    if (!fatal) {
        releaseLease(generation);
    }
}

bool LocalOtaController::queuedApplyValidLocked() const {
    return queuedApply_.pending &&
        state_ == LOCAL_OTA_READY &&
        !cancelRequested_ &&
        queuedApply_.controllerGeneration == generation_;
}

bool LocalOtaController::requestCancellationLocked(
    uint32_t expectedGeneration) {
    if (generation_ != expectedGeneration ||
        state_ == LOCAL_OTA_APPLYING ||
        state_ == LOCAL_OTA_FATAL) {
        return false;
    }
    cancelRequested_ = true;
    command_ = WORKER_CANCEL;
    return true;
}

void LocalOtaController::releaseLease(uint32_t generation) {
    network_.releaseOta(generation);
}

bool LocalOtaController::parseVersion(
    const char *version, uint16_t &major, uint16_t &minor, uint16_t &patch) {
    if (version == NULL) {
        return false;
    }
    unsigned long values[3] = {};
    const char *cursor = version;
    for (size_t component = 0; component < 3; ++component) {
        if (*cursor < '0' || *cursor > '9' ||
            (*cursor == '0' && cursor[1] >= '0' && cursor[1] <= '9')) {
            return false;
        }
        while (*cursor >= '0' && *cursor <= '9') {
            values[component] = values[component] * 10 +
                static_cast<unsigned long>(*cursor - '0');
            if (values[component] > 65535) {
                return false;
            }
            ++cursor;
        }
        if (component < 2) {
            if (*cursor++ != '.') {
                return false;
            }
        } else if (*cursor != '\0') {
            return false;
        }
    }
    major = static_cast<uint16_t>(values[0]);
    minor = static_cast<uint16_t>(values[1]);
    patch = static_cast<uint16_t>(values[2]);
    return true;
}

bool LocalOtaController::metadataAllowed(
    const OTAStagingMetadata &metadata) const {
    if (strcmp(metadata.productId, productId_) != 0 ||
        strcmp(metadata.boardId, boardId_) != 0) {
        return false;
    }
    uint16_t major, minor, patch;
    uint16_t currentMajor, currentMinor, currentPatch;
    if (!parseVersion(metadata.firmwareVersion, major, minor, patch) ||
        !parseVersion(currentVersion_, currentMajor, currentMinor, currentPatch)) {
        return false;
    }
    bool newer = major > currentMajor ||
        (major == currentMajor && minor > currentMinor) ||
        (major == currentMajor && minor == currentMinor && patch > currentPatch);
    return newer;
}
