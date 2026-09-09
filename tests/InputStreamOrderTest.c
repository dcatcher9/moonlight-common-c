#include "InputStreamTest.h"
#include "LinkedBlockingQueue.h"

#include <Limelight.h>
#include <stdio.h>
#include <stdlib.h>

// Platform.h defines NDEBUG for POSIX consumers without LC_DEBUG. Checks here
// also perform setup and enqueue work, so they must run in every build mode.
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); } } while (0)

static LiTestInputPacket pollPacket(void) {
    LiTestInputPacket packet;
    CHECK(LiTestPollInputPacket(&packet) == 1);
    return packet;
}

typedef struct _ITEM_COUNT_TEST_CONTEXT {
    PLINKED_BLOCKING_QUEUE queue;
    PLT_EVENT started;
    PLT_MUTEX completionMutex;
    bool completed;
} ITEM_COUNT_TEST_CONTEXT;

static void readItemCount(void* opaqueContext) {
    ITEM_COUNT_TEST_CONTEXT* context = opaqueContext;
    PltSetEvent(&context->started);
    (void)LbqGetItemCount(context->queue);
    PltLockMutex(&context->completionMutex);
    context->completed = true;
    PltUnlockMutex(&context->completionMutex);
}

static void testItemCountUsesQueueSynchronization(void) {
    LINKED_BLOCKING_QUEUE queue;
    ITEM_COUNT_TEST_CONTEXT context = { 0 };
    PLT_THREAD thread;

    CHECK(LbqInitializeLinkedBlockingQueue(&queue, 4) == 0);
    CHECK(PltCreateEvent(&context.started) == 0);
    CHECK(PltCreateMutex(&context.completionMutex) == 0);
    context.queue = &queue;

    PltLockMutex(&queue.mutex);
    CHECK(PltCreateThread("ItemCountTest", readItemCount, &context, &thread) == 0);
    PltWaitForEvent(&context.started);
    PltSleepMs(50);

    PltLockMutex(&context.completionMutex);
    CHECK(!context.completed);
    PltUnlockMutex(&context.completionMutex);

    PltUnlockMutex(&queue.mutex);
    PltJoinThread(&thread);
    CHECK(context.completed);

    PltDeleteMutex(&context.completionMutex);
    PltCloseEvent(&context.started);
    CHECK(LbqDestroyLinkedBlockingQueue(&queue) == NULL);
}

static void testRelativeMotionDoesNotCrossButton(void) {
    CHECK(LiTestInitializeInputStream() == 0);

    CHECK(LiSendMouseMoveEvent(1, 0) == 0);
    CHECK(LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT) == 0);
    CHECK(LiSendMouseMoveEvent(2, 0) == 0);

    LiTestInputPacket first = pollPacket();
    CHECK(first.type == LI_TEST_INPUT_PACKET_RELATIVE_MOVE);
    CHECK(first.x == 1 && first.y == 0);
    // MOVE is followed by DOWN, so the specialized relative sender must keep
    // ENet batching open for the packet which remains queued.
    CHECK(first.moreData);

    LiTestInputPacket second = pollPacket();
    CHECK(second.type == LI_TEST_INPUT_PACKET_MOUSE_BUTTON);
    CHECK(second.button == BUTTON_LEFT);

    LiTestInputPacket third = pollPacket();
    CHECK(third.type == LI_TEST_INPUT_PACKET_RELATIVE_MOVE);
    CHECK(third.x == 2 && third.y == 0);
    CHECK(!third.moreData);

    LiTestDestroyInputStream();
}

static void testAbsoluteMotionDoesNotCrossButton(void) {
    CHECK(LiTestInitializeInputStream() == 0);

    CHECK(LiSendMousePositionEvent(10, 20, 1920, 1080) == 0);
    CHECK(LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT) == 0);
    CHECK(LiSendMousePositionEvent(30, 40, 1920, 1080) == 0);

    LiTestInputPacket first = pollPacket();
    CHECK(first.type == LI_TEST_INPUT_PACKET_ABSOLUTE_MOVE);
    CHECK(first.x == 10 && first.y == 20);
    CHECK(first.width == 1920 && first.height == 1080);
    CHECK(first.moreData);

    LiTestInputPacket second = pollPacket();
    CHECK(second.type == LI_TEST_INPUT_PACKET_MOUSE_BUTTON);
    CHECK(second.button == BUTTON_LEFT);

    LiTestInputPacket third = pollPacket();
    CHECK(third.type == LI_TEST_INPUT_PACKET_ABSOLUTE_MOVE);
    CHECK(third.x == 30 && third.y == 40);
    CHECK(!third.moreData);

    LiTestDestroyInputStream();
}

static void testRelativeMotionDoesNotCrossScroll(void) {
    CHECK(LiTestInitializeInputStream() == 0);

    CHECK(LiSendMouseMoveEvent(1, 0) == 0);
    CHECK(LiSendHighResScrollEvent(120) == 0);
    CHECK(LiSendMouseMoveEvent(2, 0) == 0);

    LiTestInputPacket first = pollPacket();
    CHECK(first.type == LI_TEST_INPUT_PACKET_RELATIVE_MOVE);
    CHECK(first.x == 1 && first.y == 0);
    CHECK(first.moreData);

    LiTestInputPacket second = pollPacket();
    CHECK(second.type == LI_TEST_INPUT_PACKET_SCROLL);

    LiTestInputPacket third = pollPacket();
    CHECK(third.type == LI_TEST_INPUT_PACKET_RELATIVE_MOVE);
    CHECK(third.x == 2 && third.y == 0);

    LiTestDestroyInputStream();
}

static void testRelativeMotionDoesNotCrossKeyboard(void) {
    CHECK(LiTestInitializeInputStream() == 0);

    CHECK(LiSendMouseMoveEvent(1, 0) == 0);
    CHECK(LiSendKeyboardEvent(0x41, KEY_ACTION_DOWN, 0) == 0);
    CHECK(LiSendMouseMoveEvent(2, 0) == 0);

    LiTestInputPacket first = pollPacket();
    CHECK(first.type == LI_TEST_INPUT_PACKET_RELATIVE_MOVE);
    CHECK(first.x == 1 && first.y == 0);
    CHECK(first.moreData);

    LiTestInputPacket second = pollPacket();
    CHECK(second.type == LI_TEST_INPUT_PACKET_KEYBOARD);

    LiTestInputPacket third = pollPacket();
    CHECK(third.type == LI_TEST_INPUT_PACKET_RELATIVE_MOVE);
    CHECK(third.x == 2 && third.y == 0);

    LiTestDestroyInputStream();
}

static void testDestroyClearsPendingMouseAliases(void) {
    CHECK(LiTestInitializeInputStream() == 0);
    CHECK(LiSendMouseMoveEvent(1, 0) == 0);
    CHECK(LiTestHasPendingRelativeMouseHolder());
    CHECK(!LiTestHasPendingAbsoluteMouseHolder());

    LiTestDestroyInputStream();

    CHECK(!LiTestHasPendingRelativeMouseHolder());
    CHECK(!LiTestHasPendingAbsoluteMouseHolder());
}

static void testRelativeAccumulatorDoesNotOverflowSignedInt(void) {
    CHECK(LiTestInitializeInputStream() == 0);

    for (int i = 0; i < 65539; i++) {
        CHECK(LiSendMouseMoveEvent(INT16_MAX, 0) == 0);
    }

    LiTestInputPacket packet = pollPacket();
    CHECK(packet.type == LI_TEST_INPUT_PACKET_RELATIVE_MOVE);
    CHECK(packet.x == (int64_t)INT16_MAX * 65539);

    LiTestDestroyInputStream();
}

static void testQueueFailureDoesNotRetainFailedMouseHolder(void) {
    CHECK(LiTestInitializeInputStream() == 0);

    for (int i = 0; i < 150; i++) {
        CHECK(LiSendKeyboardEvent(0x41, KEY_ACTION_DOWN, 0) == 0);
    }
    CHECK(LiSendMouseMoveEvent(1, 0) != 0);
    CHECK(!LiTestHasPendingRelativeMouseHolder());
    CHECK(!LiTestHasPendingAbsoluteMouseHolder());

    LiTestDestroyInputStream();
}

static void testZeroNetRelativeMotionFlushesEarlierInput(void) {
    CHECK(LiTestInitializeInputStream() == 0);

    CHECK(LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT) == 0);
    CHECK(LiSendMouseMoveEvent(1, 0) == 0);
    CHECK(LiSendMouseMoveEvent(-1, 0) == 0);

    LiTestInputPacket button = pollPacket();
    CHECK(button.type == LI_TEST_INPUT_PACKET_MOUSE_BUTTON);
    CHECK(button.moreData);

    LiTestInputPacket motion = pollPacket();
    CHECK(motion.type == LI_TEST_INPUT_PACKET_RELATIVE_MOVE);
    CHECK(motion.x == 0 && motion.y == 0);
    CHECK(LiTestGetEmptyRelativeMotionFlushCount() == 1);

    LiTestDestroyInputStream();
}

int main(void) {
    testItemCountUsesQueueSynchronization();
    testRelativeMotionDoesNotCrossButton();
    testAbsoluteMotionDoesNotCrossButton();
    testRelativeMotionDoesNotCrossScroll();
    testRelativeMotionDoesNotCrossKeyboard();
    testDestroyClearsPendingMouseAliases();
    testRelativeAccumulatorDoesNotOverflowSignedInt();
    testQueueFailureDoesNotRetainFailedMouseHolder();
    testZeroNetRelativeMotionFlushesEarlierInput();
    return 0;
}
