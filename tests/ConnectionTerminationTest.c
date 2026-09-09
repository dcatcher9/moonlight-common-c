#include "Limelight-internal.h"

// Keep checks active in release builds, including the allocation/thread failure
// paths whose production diagnostics deliberately assert only in debug builds.
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); } } while (0)

static void* testMalloc(size_t size);
static void testFree(void* allocation);

// Exercise the production dispatcher and full start/stop orchestration. Only
// allocation is intercepted here; platform, network, and stream operations are
// stubbed below. No real host, transport, or scheduler timing is involved.
#define malloc testMalloc
#define free testFree
#include "../src/Connection.c"
#undef malloc
#undef free

typedef struct _QUEUED_CALLBACK {
    ThreadEntry entry;
    void* context;
} QUEUED_CALLBACK;

typedef struct _NOTIFICATION {
    int errorCode;
    uint64_t sessionId;
} NOTIFICATION;

static QUEUED_CALLBACK queuedCallbacks[16];
static int queuedCount;
static int deliveredCount;
static void* allocations[16];
static int liveAllocations;
static int allocationsExpectedInCallback;
static bool failAllocation;
static bool failThreadCreation;
static bool invokeThreadImmediately;
static int threadCreationCalls;
static NOTIFICATION notifications[16];
static int notificationCount;
static int acceptedNotifications;
static uint64_t currentSession;
static int legacyACalls;
static int legacyBCalls;
static int legacyAError;
static int legacyBError;
static bool legacyStopsConnection;
static bool streamRunning;
static int stoppedStreams;
static int startedConnections;

static void* testMalloc(size_t size) {
    if (failAllocation) {
        failAllocation = false;
        return NULL;
    }
    void* allocation = malloc(size);
    CHECK(allocation != NULL);
    for (size_t i = 0; i < sizeof(allocations) / sizeof(allocations[0]); i++) {
        if (allocations[i] == NULL) {
            allocations[i] = allocation;
            liveAllocations++;
            return allocation;
        }
    }
    CHECK(false);
    return NULL;
}

static void testFree(void* allocation) {
    if (allocation != NULL) {
        for (size_t i = 0; i < sizeof(allocations) / sizeof(allocations[0]); i++) {
            if (allocations[i] == allocation) {
                allocations[i] = NULL;
                liveAllocations--;
                break;
            }
        }
    }
    // LiStopConnection() also frees its address string, allocated by strdup().
    free(allocation);
}

int PltCreateThreadDetached(const char* name, ThreadEntry entry, void* context) {
    CHECK(strcmp(name, "AsyncTerm") == 0);
    CHECK(context != NULL);
    threadCreationCalls++;
    if (failThreadCreation) {
        failThreadCreation = false;
        return -1;
    }
    if (invokeThreadImmediately) {
        allocationsExpectedInCallback = liveAllocations - 1;
        entry(context);
        CHECK(liveAllocations == allocationsExpectedInCallback);
        return 0;
    }
    CHECK(queuedCount < (int)(sizeof(queuedCallbacks) / sizeof(queuedCallbacks[0])));
    queuedCallbacks[queuedCount].entry = entry;
    queuedCallbacks[queuedCount++].context = context;
    return 0;
}

void PltJoinThread(PLT_THREAD* thread) {
    (void)thread;
    CHECK(false); // Stopping from inside a callback must never join that callback.
}

static void deliverNext(void) {
    CHECK(deliveredCount < queuedCount);
    QUEUED_CALLBACK callback = queuedCallbacks[deliveredCount++];
    allocationsExpectedInCallback = liveAllocations - 1;
    callback.entry(callback.context);
    CHECK(liveAllocations == allocationsExpectedInCallback);
}

static void connectionStarted(void) { startedConnections++; }

static void sessionTerminated(int errorCode, uint64_t sessionId) {
    CHECK(liveAllocations == allocationsExpectedInCallback);
    CHECK(notificationCount < (int)(sizeof(notifications) / sizeof(notifications[0])));
    notifications[notificationCount].errorCode = errorCode;
    notifications[notificationCount++].sessionId = sessionId;
    // This is the client-side stale-session gate. Both sessions deliberately use
    // this exact function, so capturing just its address cannot make this safe.
    if (sessionId == currentSession) {
        acceptedNotifications++;
        LiStopConnection();
    }
}

static void legacyA(int errorCode) {
    CHECK(liveAllocations == allocationsExpectedInCallback);
    legacyACalls++;
    legacyAError = errorCode;
    if (legacyStopsConnection) {
        LiStopConnection();
    }
}

static void legacyB(int errorCode) {
    CHECK(liveAllocations == allocationsExpectedInCallback);
    legacyBCalls++;
    legacyBError = errorCode;
    if (legacyStopsConnection) {
        LiStopConnection();
    }
}

static void startConnection(uint64_t sessionId, bool sessionAware,
                            ConnListenerConnectionTerminated legacyCallback) {
    SERVER_INFORMATION server = {0};
    STREAM_CONFIGURATION config = {0};
    CONNECTION_LISTENER_CALLBACKS callbacks = {0};
    server.address = "127.0.0.1";
    server.serverInfoAppVersion = "7.1.500.0";
    server.serverCodecModeSupport = SCM_H264;
    config.width = 1920;
    config.height = 1080;
    config.fps = 60;
    config.packetSize = 1024;
    config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    config.supportedVideoFormats = VIDEO_FORMAT_H264;
    config.streamingRemotely = STREAM_CFG_LOCAL;
    callbacks.connectionStarted = connectionStarted;
    callbacks.connectionTerminated = legacyCallback;
    callbacks.connectionTerminatedWithSession = sessionAware ? sessionTerminated : NULL;
    callbacks.connectionSessionId = sessionId;

    CHECK(!streamRunning);
    currentSession = sessionId;
    int priorStarts = startedConnections;
    CHECK(LiStartConnection(&server, &config, &callbacks, NULL, NULL, NULL, 0, NULL, 0) == 0);
    CHECK(startedConnections == priorStarts + 1);
    CHECK(streamRunning && !ConnectionInterrupted);
    // Overwriting the caller's stack structure must not alter the copied session.
    memset(&callbacks, 0, sizeof(callbacks));
}

static void resetTest(void) {
    CHECK(!streamRunning);
    CHECK(liveAllocations == 0);
    CHECK(deliveredCount == queuedCount);
    queuedCount = deliveredCount = notificationCount = acceptedNotifications = 0;
    threadCreationCalls = 0;
    legacyACalls = legacyBCalls = legacyAError = legacyBError = 0;
    legacyStopsConnection = failAllocation = failThreadCreation = false;
    invokeThreadImmediately = false;
}

static void testDelayedCallbackAfterReconnect(void) {
    const uint64_t sessionA = UINT64_C(0xfedcba9876543210);
    const uint64_t sessionB = UINT64_C(0xfedcba9876543211);
    resetTest();
    startConnection(sessionA, true, legacyA);
    ListenerCallbacks.connectionTerminated(-101);
    ListenerCallbacks.connectionTerminated(-999); // Duplicate producer is suppressed.
    CHECK(queuedCount == 1 && liveAllocations == 1);
    LiStopConnection();
    startConnection(sessionB, true, legacyB);

    deliverNext();
    CHECK(notifications[0].sessionId == sessionA && notifications[0].errorCode == -101);
    CHECK(acceptedNotifications == 0 && legacyACalls == 0 && legacyBCalls == 0);
    CHECK(streamRunning && !ConnectionInterrupted);

    ListenerCallbacks.connectionTerminated(-202);
    deliverNext();
    CHECK(notifications[1].sessionId == sessionB && notifications[1].errorCode == -202);
    CHECK(acceptedNotifications == 1 && !streamRunning && ConnectionInterrupted);
    ListenerCallbacks.connectionTerminated(-999); // Stop prevents new scheduling.
    CHECK(queuedCount == 2);
    puts("PASS: delayed A retains identity/error, cannot stop B, and B can stop inside its callback");
}

static void testBothSessionsQueued(void) {
    resetTest();
    startConnection(1, true, NULL);
    ListenerCallbacks.connectionTerminated(-303);
    LiStopConnection();
    startConnection(2, true, NULL);
    ListenerCallbacks.connectionTerminated(-404);
    CHECK(liveAllocations == 2);
    deliverNext();
    CHECK(notifications[0].sessionId == 1 && notifications[0].errorCode == -303);
    CHECK(streamRunning && !ConnectionInterrupted);
    deliverNext();
    CHECK(notifications[1].sessionId == 2 && notifications[1].errorCode == -404);
    CHECK(acceptedNotifications == 1 && !streamRunning);
    puts("PASS: B scheduling cannot overwrite A's queued error or identity");
}

static void testLegacyCallbacks(void) {
    resetTest();
    startConnection(1, false, legacyA);
    ListenerCallbacks.connectionTerminated(-505);
    LiStopConnection();
    startConnection(2, false, legacyB);
    ListenerCallbacks.connectionTerminated(-606);
    deliverNext();
    CHECK(legacyACalls == 1 && legacyAError == -505 && legacyBCalls == 0);
    CHECK(streamRunning && !ConnectionInterrupted);
    legacyStopsConnection = true;
    deliverNext();
    CHECK(legacyBCalls == 1 && legacyBError == -606 && !streamRunning);

    startConnection(3, false, NULL); // Missing legacy callback uses the real default.
    ListenerCallbacks.connectionTerminated(0);
    deliverNext();
    CHECK(streamRunning);
    LiStopConnection();
    puts("PASS: legacy callback capture, default callback, and stop inside legacy callback");
}

static void testSchedulingFailures(void) {
    resetTest();
    startConnection(1, true, NULL);
    failAllocation = true;
    ListenerCallbacks.connectionTerminated(-707);
    CHECK(queuedCount == 0 && threadCreationCalls == 0 && liveAllocations == 0);
    ListenerCallbacks.connectionTerminated(-708);
    CHECK(threadCreationCalls == 0);
    LiStopConnection();

    startConnection(2, true, NULL);
    failThreadCreation = true;
    ListenerCallbacks.connectionTerminated(-808);
    CHECK(queuedCount == 0 && threadCreationCalls == 1 && liveAllocations == 0);
    LiStopConnection();

    startConnection(3, true, NULL);
    ListenerCallbacks.connectionTerminated(-909);
    deliverNext();
    CHECK(notifications[0].sessionId == 3 && notifications[0].errorCode == -909);
    CHECK(!streamRunning && liveAllocations == 0);
    puts("PASS: failed allocation/create releases ownership and later sessions still terminate");
}

static void testCallbackBeforeThreadCreationReturns(void) {
    resetTest();
    startConnection(42, true, legacyA);
    invokeThreadImmediately = true;
    ListenerCallbacks.connectionTerminated(-1001);
    CHECK(threadCreationCalls == 1 && queuedCount == 0 && liveAllocations == 0);
    CHECK(notifications[0].sessionId == 42 && notifications[0].errorCode == -1001);
    CHECK(acceptedNotifications == 1 && legacyACalls == 0);
    CHECK(!streamRunning && ConnectionInterrupted);
    ListenerCallbacks.connectionTerminated(-1002);
    CHECK(threadCreationCalls == 1);
    puts("PASS: callback may free its context and stop before thread creation returns");
}

// Successful lifecycle stubs keep the production LiStartConnection() callback
// registration, state reset, stage traversal, and LiStopConnection() intact.
int extractVersionQuadFromString(const char* version, int* quad) {
    return sscanf(version, "%d.%d.%d.%d", &quad[0], &quad[1], &quad[2], &quad[3]) == 4 ? 0 : -1;
}
int initializePlatform(void) { return 0; }
void cleanupPlatform(void) {}
int resolveHostName(const char* host, int family, int port, struct sockaddr_storage* address, SOCKADDR_LEN* length) {
    (void)host; (void)family; (void)port;
    memset(address, 0, sizeof(*address));
    address->ss_family = AF_INET;
    *length = sizeof(struct sockaddr_in);
    return 0;
}
bool isPrivateNetworkAddress(struct sockaddr_storage* address) { (void)address; return true; }
bool isNat64SynthesizedAddress(struct sockaddr_storage* address) { (void)address; return false; }
int initializeAudioStream(void) { return 0; }
int performRtspHandshake(PSERVER_INFORMATION server) { (void)server; return 0; }
int initializeControlStream(void) { return 0; }
void initializeVideoStream(void) {}
int initializeInputStream(void) { return 0; }
int startControlStream(void) { return 0; }
int startVideoStream(void* context, int flags) { (void)context; (void)flags; return 0; }
int startAudioStream(void* context, int flags) { (void)context; (void)flags; return 0; }
int startInputStream(void) { streamRunning = true; return 0; }
int stopInputStream(void) { CHECK(streamRunning); streamRunning = false; stoppedStreams++; return 0; }
void stopAudioStream(void) {}
void stopVideoStream(void) {}
int stopControlStream(void) { return 0; }
void destroyInputStream(void) {}
void destroyVideoStream(void) {}
void destroyControlStream(void) {}
void destroyAudioStream(void) {}
int LiSendMouseMoveEvent(short x, short y) { (void)x; (void)y; return 0; }
void PltSleepMs(int ms) { (void)ms; }

int main(void) {
    testDelayedCallbackAfterReconnect();
    testBothSessionsQueued();
    testLegacyCallbacks();
    testSchedulingFailures();
    testCallbackBeforeThreadCreationReturns();
    resetTest();
    CHECK(stoppedStreams == startedConnections);
    puts("Connection termination tests passed (controlled dispatch, stubbed transport).");
    return 0;
}
