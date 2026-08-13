#include "InputStreamTest.h"
#include "LinkedBlockingQueue.h"

#include <Limelight.h>
#include <assert.h>

static LiTestInputPacket pollPacket(void) {
    LiTestInputPacket packet;
    assert(LiTestPollInputPacket(&packet) == 1);
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

    assert(LbqInitializeLinkedBlockingQueue(&queue, 4) == 0);
    assert(PltCreateEvent(&context.started) == 0);
    assert(PltCreateMutex(&context.completionMutex) == 0);
    context.queue = &queue;

    PltLockMutex(&queue.mutex);
    assert(PltCreateThread("ItemCountTest", readItemCount, &context, &thread) == 0);
    PltWaitForEvent(&context.started);
    PltSleepMs(50);

    PltLockMutex(&context.completionMutex);
    assert(!context.completed);
    PltUnlockMutex(&context.completionMutex);

    PltUnlockMutex(&queue.mutex);
    PltJoinThread(&thread);
    assert(context.completed);

    PltDeleteMutex(&context.completionMutex);
    PltCloseEvent(&context.started);
    assert(LbqDestroyLinkedBlockingQueue(&queue) == NULL);
}

static void testRelativeMotionDoesNotCrossButton(void) {
    assert(LiTestInitializeInputStream() == 0);

    assert(LiSendMouseMoveEvent(1, 0) == 0);
    assert(LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT) == 0);
    assert(LiSendMouseMoveEvent(2, 0) == 0);

    LiTestInputPacket first = pollPacket();
    assert(first.type == LI_TEST_INPUT_PACKET_RELATIVE_MOVE);
    assert(first.x == 1 && first.y == 0);
    // MOVE is followed by DOWN, so the specialized relative sender must keep
    // ENet batching open for the packet which remains queued.
    assert(first.moreData);

    LiTestInputPacket second = pollPacket();
    assert(second.type == LI_TEST_INPUT_PACKET_MOUSE_BUTTON);
    assert(second.button == BUTTON_LEFT);

    LiTestInputPacket third = pollPacket();
    assert(third.type == LI_TEST_INPUT_PACKET_RELATIVE_MOVE);
    assert(third.x == 2 && third.y == 0);
    assert(!third.moreData);

    LiTestDestroyInputStream();
}

static void testAbsoluteMotionDoesNotCrossButton(void) {
    assert(LiTestInitializeInputStream() == 0);

    assert(LiSendMousePositionEvent(10, 20, 1920, 1080) == 0);
    assert(LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT) == 0);
    assert(LiSendMousePositionEvent(30, 40, 1920, 1080) == 0);

    LiTestInputPacket first = pollPacket();
    assert(first.type == LI_TEST_INPUT_PACKET_ABSOLUTE_MOVE);
    assert(first.x == 10 && first.y == 20);
    assert(first.width == 1920 && first.height == 1080);
    assert(first.moreData);

    LiTestInputPacket second = pollPacket();
    assert(second.type == LI_TEST_INPUT_PACKET_MOUSE_BUTTON);
    assert(second.button == BUTTON_LEFT);

    LiTestInputPacket third = pollPacket();
    assert(third.type == LI_TEST_INPUT_PACKET_ABSOLUTE_MOVE);
    assert(third.x == 30 && third.y == 40);
    assert(!third.moreData);

    LiTestDestroyInputStream();
}

static void testRelativeMotionDoesNotCrossScroll(void) {
    assert(LiTestInitializeInputStream() == 0);

    assert(LiSendMouseMoveEvent(1, 0) == 0);
    assert(LiSendHighResScrollEvent(120) == 0);
    assert(LiSendMouseMoveEvent(2, 0) == 0);

    LiTestInputPacket first = pollPacket();
    assert(first.type == LI_TEST_INPUT_PACKET_RELATIVE_MOVE);
    assert(first.x == 1 && first.y == 0);
    assert(first.moreData);

    LiTestInputPacket second = pollPacket();
    assert(second.type == LI_TEST_INPUT_PACKET_SCROLL);

    LiTestInputPacket third = pollPacket();
    assert(third.type == LI_TEST_INPUT_PACKET_RELATIVE_MOVE);
    assert(third.x == 2 && third.y == 0);

    LiTestDestroyInputStream();
}

static void testRelativeMotionDoesNotCrossKeyboard(void) {
    assert(LiTestInitializeInputStream() == 0);

    assert(LiSendMouseMoveEvent(1, 0) == 0);
    assert(LiSendKeyboardEvent(0x41, KEY_ACTION_DOWN, 0) == 0);
    assert(LiSendMouseMoveEvent(2, 0) == 0);

    LiTestInputPacket first = pollPacket();
    assert(first.type == LI_TEST_INPUT_PACKET_RELATIVE_MOVE);
    assert(first.x == 1 && first.y == 0);
    assert(first.moreData);

    LiTestInputPacket second = pollPacket();
    assert(second.type == LI_TEST_INPUT_PACKET_KEYBOARD);

    LiTestInputPacket third = pollPacket();
    assert(third.type == LI_TEST_INPUT_PACKET_RELATIVE_MOVE);
    assert(third.x == 2 && third.y == 0);

    LiTestDestroyInputStream();
}

static void testDestroyClearsPendingMouseAliases(void) {
    assert(LiTestInitializeInputStream() == 0);
    assert(LiSendMouseMoveEvent(1, 0) == 0);
    assert(LiTestHasPendingRelativeMouseHolder());
    assert(!LiTestHasPendingAbsoluteMouseHolder());

    LiTestDestroyInputStream();

    assert(!LiTestHasPendingRelativeMouseHolder());
    assert(!LiTestHasPendingAbsoluteMouseHolder());
}

static void testRelativeAccumulatorDoesNotOverflowSignedInt(void) {
    assert(LiTestInitializeInputStream() == 0);

    for (int i = 0; i < 65539; i++) {
        assert(LiSendMouseMoveEvent(INT16_MAX, 0) == 0);
    }

    LiTestInputPacket packet = pollPacket();
    assert(packet.type == LI_TEST_INPUT_PACKET_RELATIVE_MOVE);
    assert(packet.x == (int64_t)INT16_MAX * 65539);

    LiTestDestroyInputStream();
}

static void testQueueFailureDoesNotRetainFailedMouseHolder(void) {
    assert(LiTestInitializeInputStream() == 0);

    for (int i = 0; i < 150; i++) {
        assert(LiSendKeyboardEvent(0x41, KEY_ACTION_DOWN, 0) == 0);
    }
    assert(LiSendMouseMoveEvent(1, 0) != 0);
    assert(!LiTestHasPendingRelativeMouseHolder());
    assert(!LiTestHasPendingAbsoluteMouseHolder());

    LiTestDestroyInputStream();
}

static void testZeroNetRelativeMotionFlushesEarlierInput(void) {
    assert(LiTestInitializeInputStream() == 0);

    assert(LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT) == 0);
    assert(LiSendMouseMoveEvent(1, 0) == 0);
    assert(LiSendMouseMoveEvent(-1, 0) == 0);

    LiTestInputPacket button = pollPacket();
    assert(button.type == LI_TEST_INPUT_PACKET_MOUSE_BUTTON);
    assert(button.moreData);

    LiTestInputPacket motion = pollPacket();
    assert(motion.type == LI_TEST_INPUT_PACKET_RELATIVE_MOVE);
    assert(motion.x == 0 && motion.y == 0);
    assert(LiTestGetEmptyRelativeMotionFlushCount() == 1);

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
