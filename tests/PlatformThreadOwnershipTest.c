#define _GNU_SOURCE
#include "Limelight-internal.h"

#if defined(__vita__) || defined(__WIIU__) || defined(__3DS__)
#error This real-thread test currently supports Windows, Darwin, and desktop POSIX only
#endif

// Checks must remain active in release builds.
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); } } while (0)

static void* testMalloc(size_t size);
static void testFree(void* allocation);
#ifdef LC_WINDOWS
static HANDLE WINAPI testCreateThread(LPSECURITY_ATTRIBUTES attributes, SIZE_T stackSize,
                                      LPTHREAD_START_ROUTINE entry, LPVOID context,
                                      DWORD flags, LPDWORD threadId);
#else
static int testPthreadCreate(pthread_t* thread, const pthread_attr_t* attributes,
                             void* (*entry)(void*), void* context);
#endif

// Only allocations and a requested thread-creation failure are intercepted.
// All successful starts use the actual platform thread implementation.
#define malloc testMalloc
#define free testFree
#ifdef LC_WINDOWS
#define CreateThread testCreateThread
#else
#define pthread_create testPthreadCreate
#endif
#include "../src/Platform.c"
#undef malloc
#undef free
#ifdef LC_WINDOWS
#undef CreateThread
#else
#undef pthread_create
#endif

static PLT_MUTEX allocationLock;
static void* trackedAllocation;
static unsigned allocations;
static unsigned frees;
static bool failAllocation;
static bool failThreadCreation;

static void* testMalloc(size_t size) {
    if (failAllocation) {
        failAllocation = false;
        return NULL;
    }
    void* allocation = malloc(size);
    CHECK(allocation != NULL);
    PltLockMutex(&allocationLock);
    CHECK(trackedAllocation == NULL);
    trackedAllocation = allocation;
    allocations++;
    PltUnlockMutex(&allocationLock);
    return allocation;
}

static void testFree(void* allocation) {
    PltLockMutex(&allocationLock);
    // Reject double frees and attempts to free the entry's borrowed context.
    CHECK(allocation != NULL && allocation == trackedAllocation);
    trackedAllocation = NULL;
    frees++;
    PltUnlockMutex(&allocationLock);
    free(allocation);
}

#ifdef LC_WINDOWS
static HANDLE WINAPI testCreateThread(LPSECURITY_ATTRIBUTES attributes, SIZE_T stackSize,
                                      LPTHREAD_START_ROUTINE entry, LPVOID context,
                                      DWORD flags, LPDWORD threadId) {
    if (failThreadCreation) {
        failThreadCreation = false;
        return NULL;
    }
    return CreateThread(attributes, stackSize, entry, context, flags, threadId);
}
#else
static int testPthreadCreate(pthread_t* thread, const pthread_attr_t* attributes,
                             void* (*entry)(void*), void* context) {
    if (failThreadCreation) {
        failThreadCreation = false;
        return EAGAIN;
    }
    return pthread_create(thread, attributes, entry, context);
}
#endif

typedef struct _ENTRY_CONTEXT {
    PLT_MUTEX lock;
    PLT_COND condition;
    bool finished;
    bool exitDirectly;
    int value;
} ENTRY_CONTEXT;

static void checkNoLaunchAllocation(void) {
    PltLockMutex(&allocationLock);
    CHECK(trackedAllocation == NULL);
    CHECK(allocations == frees);
    PltUnlockMutex(&allocationLock);
}

static void realThreadEntry(void* opaque) {
    ENTRY_CONTEXT* context = opaque;
    // Require the trampoline to free before dispatch, including entries that
    // leave via ExitThread()/pthread_exit() and never return to ThreadProc().
    checkNoLaunchAllocation();
    PltLockMutex(&context->lock);
    CHECK(context->value == 17);
    context->value = 29;
    bool exitDirectly = context->exitDirectly;
    context->finished = true;
    PltSignalConditionVariable(&context->condition);
    PltUnlockMutex(&context->lock);
    // No further access to the caller's context is made after unlocking.
    if (exitDirectly) {
#ifdef LC_WINDOWS
        ExitThread(0);
#else
        pthread_exit(NULL);
#endif
    }
}

static void runRealThread(bool detached, bool exitDirectly) {
    ENTRY_CONTEXT context = {0};
    context.exitDirectly = exitDirectly;
    context.value = 17;
    CHECK(PltCreateMutex(&context.lock) == 0);
    CHECK(PltCreateConditionVariable(&context.condition, &context.lock) == 0);

    PLT_THREAD thread;
    if (detached) {
        CHECK(PltCreateThreadDetached("Ownership", realThreadEntry, &context) == 0);
    }
    else {
        CHECK(PltCreateThread("Ownership", realThreadEntry, &context, &thread) == 0);
    }

    PltLockMutex(&context.lock);
    while (!context.finished) {
        PltWaitForConditionVariable(&context.condition, &context.lock);
    }
    CHECK(context.value == 29);
    PltUnlockMutex(&context.lock);
    if (!detached) {
        PltJoinThread(&thread);
    }
    checkNoLaunchAllocation();
    CHECK(activeThreads == 0);
    PltDeleteConditionVariable(&context.condition);
    PltDeleteMutex(&context.lock);
}

static void unexpectedEntry(void* context) {
    CHECK(false);
}

static void runFailureCases(bool detached) {
    int callerContext = 17;
    PLT_THREAD thread;
    unsigned before = allocations;
    failAllocation = true;
    int result = detached
        ? PltCreateThreadDetached("AllocationFail", unexpectedEntry, &callerContext)
        : PltCreateThread("AllocationFail", unexpectedEntry, &callerContext, &thread);
    CHECK(result != 0 && allocations == before);
    CHECK(callerContext == 17);
    checkNoLaunchAllocation();

    failThreadCreation = true;
    result = detached
        ? PltCreateThreadDetached("CreationFail", unexpectedEntry, &callerContext)
        : PltCreateThread("CreationFail", unexpectedEntry, &callerContext, &thread);
    CHECK(result != 0 && allocations == before + 1);
    CHECK(callerContext == 17 && activeThreads == 0);
    checkNoLaunchAllocation();
}

// Platform subsystem initialization is outside this test; real OS thread and
// synchronization functions above are exercised without network/ENet startup.
int initializePlatformSockets(void) { return 0; }
void cleanupPlatformSockets(void) {}
void enterLowLatencyMode(void) {}
void exitLowLatencyMode(void) {}
int enet_initialize(void) { return 0; }
void enet_deinitialize(void) {}

int main(void) {
    CHECK(PltCreateMutex(&allocationLock) == 0);
    for (int i = 0; i < 32; i++) {
        runRealThread(false, false);
        runRealThread(true, false);
        runRealThread(false, true);
        runRealThread(true, true);
    }
    runFailureCases(false);
    runFailureCases(true);
    checkNoLaunchAllocation();
    PltDeleteMutex(&allocationLock);
    CHECK(activeThreads == 0 && activeMutexes == 0 && activeCondVars == 0);
    printf("Platform threads: %u launch contexts freed once; real joined/detached threads and failure paths passed\n", frees);
    return 0;
}
