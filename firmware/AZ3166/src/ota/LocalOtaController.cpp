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

uint8_t hexNibble(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<uint8_t>(value - '0');
    }
    return static_cast<uint8_t>(value - 'a' + 10);
}

bool decodeAuthorization(const char *value, uint8_t output[16]) {
    if (value == NULL || strlen(value) != 36 || memcmp(value, "OTA ", 4) != 0) {
        return false;
    }
    for (size_t index = 0; index < 32; ++index) {
        char current = value[index + 4];
        if (!((current >= '0' && current <= '9') ||
              (current >= 'a' && current <= 'f'))) {
            return false;
        }
    }
    for (size_t index = 0; index < 16; ++index) {
        output[index] = static_cast<uint8_t>(
            (hexNibble(value[4 + index * 2]) << 4) |
            hexNibble(value[5 + index * 2]));
    }
    return true;
}

}

LocalOtaController::LocalOtaController(
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
    LocalOtaBeforeApplyValidation beforeApplyValidation,
    void *applyValidationContext)
    : core_(core),
      entropy_(entropy),
      display_(display),
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
      peerAddress_(0),
      challengeDeadline_(0),
      capabilityDeadline_(0),
      leaseDeadline_(0),
      challenge_{},
      capability_{},
      failedClaims_(0),
      cancelRequested_(false),
      uploadCompleted_(false),
      applyCompleted_(false),
      activationAwaitingResponse_(false),
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

bool LocalOtaController::openChallenge() {
    uint8_t challenge[4];
    if (publicKeySize_ == 0 || !entropy_.fill(challenge, sizeof(challenge))) {
        return false;
    }
    char text[9];
    snprintf(text, sizeof(text), "%02x%02x%02x%02x",
             challenge[0], challenge[1], challenge[2], challenge[3]);
    uint32_t generation;
    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        if (activationAwaitingResponse_ ||
            (state_ != LOCAL_OTA_IDLE && state_ != LOCAL_OTA_ERROR)) {
            return false;
        }
        ++generation_;
        if (generation_ == 0) {
            ++generation_;
        }
        memcpy(challenge_, challenge, sizeof(challenge_));
        clearCapabilityLocked();
        failedClaims_ = 0;
        challengeDeadline_ = clock_() + CHALLENGE_WINDOW_MS;
        lastError_ = OTA_OK;
        state_ = LOCAL_OTA_CHALLENGE_DISPLAYED;
        generation = generation_;
    }
    showChallengeIfCurrent(generation, text);
    return true;
}

bool LocalOtaController::claim(
    const char *challenge,
    uint32_t peerAddress,
    uint32_t networkGeneration,
    char capabilityHex[33]) {
    uint8_t expected[4];
    if (challenge == NULL || strlen(challenge) != 8 ||
        peerAddress == 0 || capabilityHex == NULL) {
        return false;
    }
    for (size_t index = 0; index < 8; ++index) {
        char value = challenge[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) {
            return false;
        }
    }
    for (size_t index = 0; index < 4; ++index) {
        expected[index] = static_cast<uint8_t>(
            (hexNibble(challenge[index * 2]) << 4) |
            hexNibble(challenge[index * 2 + 1]));
    }

    uint32_t attemptGeneration = 0;
    bool lockout = false;
    bool accepted = false;
    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        attemptGeneration = generation_;
        uint8_t difference = 0;
        for (size_t index = 0; index < sizeof(challenge_); ++index) {
            difference |= static_cast<uint8_t>(challenge_[index] ^ expected[index]);
        }
        if (state_ != LOCAL_OTA_CHALLENGE_DISPLAYED ||
            elapsed(clock_(), challengeDeadline_) ||
            failedClaims_ >= 3 || difference != 0) {
            if (state_ == LOCAL_OTA_CHALLENGE_DISPLAYED &&
                !elapsed(clock_(), challengeDeadline_) &&
                failedClaims_ < 3) {
                ++failedClaims_;
                if (failedClaims_ == 3) {
                    memset(challenge_, 0, sizeof(challenge_));
                    state_ = LOCAL_OTA_IDLE;
                    lockout = true;
                }
            }
        } else {
            accepted = true;
        }
    }
    if (accepted) {
        lockout = false;
    } else {
        if (lockout) {
            clearChallengeIfCurrent(attemptGeneration);
        }
        return false;
    }

    uint8_t capability[16];
    if (!entropy_.fill(capability, sizeof(capability))) {
        {
            std::lock_guard<rtos::Mutex> lock(mutex_);
            state_ = LOCAL_OTA_IDLE;
            memset(challenge_, 0, sizeof(challenge_));
        }
        clearChallengeIfCurrent(attemptGeneration);
        return false;
    }

    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        if (state_ != LOCAL_OTA_CHALLENGE_DISPLAYED ||
            elapsed(clock_(), challengeDeadline_)) {
            return false;
        }
        memcpy(capability_, capability, sizeof(capability_));
        memset(challenge_, 0, sizeof(challenge_));
        peerAddress_ = peerAddress;
        networkGeneration_ = networkGeneration;
        capabilityDeadline_ = clock_() + CAPABILITY_WINDOW_MS;
        state_ = LOCAL_OTA_ARMED;
    }
    static const char HEX[] = "0123456789abcdef";
    for (size_t index = 0; index < sizeof(capability); ++index) {
        capabilityHex[index * 2] = HEX[capability[index] >> 4];
        capabilityHex[index * 2 + 1] = HEX[capability[index] & 0x0f];
    }
    capabilityHex[32] = '\0';
    clearChallengeIfCurrent(attemptGeneration);
    return true;
}

void LocalOtaController::showChallengeIfCurrent(
    uint32_t generation, const char *text) {
    std::lock_guard<rtos::Mutex> displayLock(displayMutex_);
    bool current;
    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        current = generation_ == generation &&
            state_ == LOCAL_OTA_CHALLENGE_DISPLAYED;
    }
    if (current) {
        display_.showChallenge(text);
    } else {
        display_.clearChallenge();
    }
}

void LocalOtaController::clearChallengeIfCurrent(uint32_t generation) {
    std::lock_guard<rtos::Mutex> displayLock(displayMutex_);
    bool clear;
    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        clear = generation_ == generation &&
            state_ != LOCAL_OTA_CHALLENGE_DISPLAYED;
    }
    if (clear) {
        display_.clearChallenge();
    }
}

bool LocalOtaController::capabilityValidLocked(
    const LocalHttpRequest &request,
    uint32_t now,
    uint32_t &generation) const {
    uint8_t candidate[16] = {};
    bool syntaxValid =
        request.authorizationCount == 1 &&
        decodeAuthorization(request.authorization, candidate);
    uint8_t difference = 0;
    for (size_t index = 0; index < sizeof(capability_); ++index) {
        difference |= static_cast<uint8_t>(candidate[index] ^ capability_[index]);
    }
    bool active = state_ == LOCAL_OTA_ARMED ||
        state_ == LOCAL_OTA_WAITING_FOR_NETWORK_LEASE ||
        state_ == LOCAL_OTA_RECEIVING ||
        state_ == LOCAL_OTA_VERIFYING ||
        state_ == LOCAL_OTA_READY ||
        state_ == LOCAL_OTA_APPLYING ||
        state_ == LOCAL_OTA_ERROR;
    generation = generation_;
    return syntaxValid && active && !elapsed(now, capabilityDeadline_) &&
        request.peerAddress == peerAddress_ &&
        request.networkGeneration == networkGeneration_ &&
        difference == 0;
}

bool LocalOtaController::authorize(
    const LocalHttpRequest &request,
    uint32_t &generation) {
    std::lock_guard<rtos::Mutex> lock(mutex_);
    return capabilityValidLocked(request, clock_(), generation);
}

bool LocalOtaController::upload(
    const LocalHttpStreamingRequest &request,
    LocalHttpBodyStream &body) {
    uint32_t authorizedGeneration;
    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        if (!capabilityValidLocked(
                request.metadata, clock_(), authorizedGeneration) ||
            state_ != LOCAL_OTA_ARMED ||
            request.contentLength < OTA_PACKAGE_PAYLOAD_OFFSET + 1 ||
            command_ != WORKER_NONE ||
            !network_.reserveOta(authorizedGeneration)) {
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
                wake = requestCancellationLocked(authorizedGeneration);
            }
        }
        if (wake) {
            commandSignal_.release();
        }
    }
}

bool LocalOtaController::apply(
    const LocalHttpRequest &request,
    OTAStagingError &result) {
    uint32_t generation;
    uint8_t capability[16] = {};
    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        if (!capabilityValidLocked(request, clock_(), generation) ||
            !decodeAuthorization(request.authorization, capability) ||
            state_ != LOCAL_OTA_READY || command_ != WORKER_NONE) {
            return false;
        }
        applyCompleted_ = false;
        applyResult_ = OTA_ERROR_INVALID_STATE;
        queuedApply_.pending = true;
        memcpy(queuedApply_.capability, capability, sizeof(capability));
        queuedApply_.peerAddress = request.peerAddress;
        queuedApply_.controllerGeneration = generation;
        queuedApply_.networkGeneration = request.networkGeneration;
        queuedApply_.deadline = capabilityDeadline_;
        command_ = WORKER_APPLY;
    }
    commandSignal_.release();
    if (applyCompletion_.wait(APPLY_WAIT_MS) <= 0) {
        fail(OTA_ERROR_ACTIVATION_UNCERTAIN, true);
        result = OTA_ERROR_ACTIVATION_UNCERTAIN;
        return true;
    }
    std::lock_guard<rtos::Mutex> lock(mutex_);
    result = applyResult_;
    return applyCompleted_;
}

bool LocalOtaController::cancel(const LocalHttpRequest &request) {
    uint32_t generation;
    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        if (!capabilityValidLocked(request, clock_(), generation) ||
            state_ == LOCAL_OTA_APPLYING || state_ == LOCAL_OTA_FATAL) {
            return false;
        }
        requestCancellationLocked(generation);
    }
    commandSignal_.release();
    return true;
}

void LocalOtaController::update(uint32_t networkGeneration) {
    uint32_t now = clock_();
    uint32_t generation = 0;
    bool clearDisplay = false;
    bool wake = false;
    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        generation = generation_;
        if (state_ == LOCAL_OTA_CHALLENGE_DISPLAYED &&
            elapsed(now, challengeDeadline_)) {
            memset(challenge_, 0, sizeof(challenge_));
            state_ = LOCAL_OTA_IDLE;
            clearDisplay = true;
        } else if (state_ == LOCAL_OTA_ERROR &&
                   elapsed(now, capabilityDeadline_)) {
            clearCapabilityLocked();
            state_ = LOCAL_OTA_IDLE;
        } else if ((state_ == LOCAL_OTA_ARMED ||
                    state_ == LOCAL_OTA_WAITING_FOR_NETWORK_LEASE ||
                    state_ == LOCAL_OTA_RECEIVING ||
                    state_ == LOCAL_OTA_VERIFYING ||
                    state_ == LOCAL_OTA_READY) &&
                   (elapsed(now, capabilityDeadline_) ||
                    networkGeneration != networkGeneration_ ||
                    (state_ == LOCAL_OTA_WAITING_FOR_NETWORK_LEASE &&
                     elapsed(now, leaseDeadline_)))) {
            wake = requestCancellationLocked(generation_);
        } else if (state_ == LOCAL_OTA_WAITING_FOR_NETWORK_LEASE &&
                   network_.tryGrantOta(generation_)) {
            state_ = LOCAL_OTA_RECEIVING;
            wake = true;
        }
    }
    if (clearDisplay) {
        clearChallengeIfCurrent(generation);
    }
    if (wake) {
        commandSignal_.release();
    }
    (void) generation;
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
        "Disabled", "Idle", "ChallengeDisplayed", "Armed",
        "WaitingForNetworkLease", "Receiving", "Verifying", "Ready",
        "Applying", "Error", "Fatal"
    };
    return state >= LOCAL_OTA_DISABLED && state <= LOCAL_OTA_FATAL
        ? NAMES[state] : "Unknown";
}

const char *LocalOtaController::errorName(OTAStagingError error) {
    return core_.errorName(error);
}

void LocalOtaController::responseAttempted(bool) {
    uint32_t generation = 0;
    {
        std::lock_guard<rtos::Mutex> lock(mutex_);
        if (activationAwaitingResponse_) {
            activationAwaitingResponse_ = false;
            rebootPending_ = true;
            generation = generation_;
        }
    }
    if (generation != 0) {
        releaseLease(generation);
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
    return metadata != NULL &&
        static_cast<LocalOtaController *>(context)->metadataAllowed(*metadata)
        ? 0 : -1;
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
            result = readStatus == LOCAL_HTTP_BODY_CANCELLED
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
            std::lock_guard<rtos::Mutex> lock(mutex_);
            staged_ = staged;
            state_ = LOCAL_OTA_READY;
            lastError_ = OTA_OK;
            command_ = WORKER_NONE;
            uploadBody_ = NULL;
            uploadCompleted_ = true;
            uploadCompletion_.release();
            return;
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
        if (!queuedApplyValidLocked(clock_())) {
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
            clearCapabilityLocked();
            state_ = LOCAL_OTA_IDLE;
            activationAwaitingResponse_ = true;
        } else if (result == OTA_ERROR_ACTIVATION_UNCERTAIN) {
            clearCapabilityLocked();
            state_ = LOCAL_OTA_FATAL;
        } else {
            clearCapabilityLocked();
            state_ = LOCAL_OTA_ERROR;
        }
    }
    if (result != OTA_OK && result != OTA_ERROR_ACTIVATION_UNCERTAIN) {
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
            clearCapabilityLocked();
            state_ = LOCAL_OTA_FATAL;
        } else if (error == OTA_ERROR_CANCELLED) {
            clearCapabilityLocked();
            state_ = LOCAL_OTA_IDLE;
        } else {
            state_ = LOCAL_OTA_ERROR;
        }
    }
    if (!fatal) {
        releaseLease(generation);
    }
}

bool LocalOtaController::queuedApplyValidLocked(uint32_t now) const {
    uint8_t difference = 0;
    for (size_t index = 0; index < sizeof(capability_); ++index) {
        difference |= static_cast<uint8_t>(
            queuedApply_.capability[index] ^ capability_[index]);
    }
    return queuedApply_.pending &&
        state_ == LOCAL_OTA_READY &&
        !cancelRequested_ &&
        queuedApply_.peerAddress == peerAddress_ &&
        queuedApply_.controllerGeneration == generation_ &&
        queuedApply_.networkGeneration == networkGeneration_ &&
        queuedApply_.deadline == capabilityDeadline_ &&
        !elapsed(now, queuedApply_.deadline) &&
        difference == 0;
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

void LocalOtaController::clearCapabilityLocked() {
    memset(capability_, 0, sizeof(capability_));
    peerAddress_ = 0;
    capabilityDeadline_ = 0;
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
    return major > currentMajor ||
        (major == currentMajor && minor > currentMinor) ||
        (major == currentMajor && minor == currentMinor && patch > currentPatch);
}
