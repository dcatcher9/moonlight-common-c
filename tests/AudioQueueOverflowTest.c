#include "LinkedBlockingQueue.h"

// Platform.h defines NDEBUG in non-debug POSIX consumers. Keep every check and
// its queue operation active regardless of the library/test build configuration.
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); } } while (0)

typedef struct _TEST_QUEUE_ITEM {
    LINKED_BLOCKING_QUEUE_ENTRY entry;
    int value;
} TEST_QUEUE_ITEM;

static TEST_QUEUE_ITEM* pollItem(PLINKED_BLOCKING_QUEUE queue) {
    TEST_QUEUE_ITEM* item;
    CHECK(LbqPollQueueElement(queue, (void**)&item) == LBQ_SUCCESS);
    return item;
}

int main(void) {
    LINKED_BLOCKING_QUEUE queue;
    LINKED_BLOCKING_QUEUE singleItemQueue;
    LINKED_BLOCKING_QUEUE unboundedQueue;
    TEST_QUEUE_ITEM items[] = {
        { .value = 1 },
        { .value = 2 },
        { .value = 3 },
        { .value = 4 },
    };
    TEST_QUEUE_ITEM* evictedItem;

    CHECK(LbqInitializeLinkedBlockingQueue(&queue, 3) == 0);
    CHECK(LbqOfferQueueItem(&queue, &items[0], &items[0].entry) == LBQ_SUCCESS);
    CHECK(LbqOfferQueueItem(&queue, &items[1], &items[1].entry) == LBQ_SUCCESS);
    CHECK(LbqOfferQueueItem(&queue, &items[2], &items[2].entry) == LBQ_SUCCESS);

    CHECK(LbqOfferQueueItemWithHeadEviction(&queue,
                                             &items[3],
                                             &items[3].entry,
                                             (void**)&evictedItem) == LBQ_SUCCESS);
    CHECK(evictedItem == &items[0]);
    CHECK(LbqGetItemCount(&queue) == 3);

    // A saturated audio queue must lose exactly the oldest packet while
    // preserving the order of the complete newest latency window.
    CHECK(pollItem(&queue)->value == 2);
    CHECK(pollItem(&queue)->value == 3);
    CHECK(pollItem(&queue)->value == 4);
    CHECK(LbqPollQueueElement(&queue, (void**)&evictedItem) == LBQ_NO_ELEMENT);

    LbqSignalQueueDrain(&queue);
    CHECK(LbqDestroyLinkedBlockingQueue(&queue) == NULL);

    CHECK(LbqInitializeLinkedBlockingQueue(&singleItemQueue, 1) == 0);
    CHECK(LbqOfferQueueItem(&singleItemQueue, &items[0], &items[0].entry) == LBQ_SUCCESS);
    CHECK(LbqOfferQueueItemWithHeadEviction(&singleItemQueue,
                                             &items[1],
                                             &items[1].entry,
                                             (void**)&evictedItem) == LBQ_SUCCESS);
    CHECK(evictedItem == &items[0]);
    CHECK(pollItem(&singleItemQueue)->value == 2);
    LbqSignalQueueDrain(&singleItemQueue);
    CHECK(LbqDestroyLinkedBlockingQueue(&singleItemQueue) == NULL);

    // A zero bound is the generic LBQ convention for an unbounded queue.
    CHECK(LbqInitializeLinkedBlockingQueue(&unboundedQueue, 0) == 0);
    CHECK(LbqOfferQueueItem(&unboundedQueue, &items[0], &items[0].entry) == LBQ_SUCCESS);
    CHECK(LbqOfferQueueItemWithHeadEviction(&unboundedQueue,
                                             &items[1],
                                             &items[1].entry,
                                             (void**)&evictedItem) == LBQ_SUCCESS);
    CHECK(evictedItem == NULL);
    CHECK(pollItem(&unboundedQueue)->value == 1);
    CHECK(pollItem(&unboundedQueue)->value == 2);
    LbqSignalQueueDrain(&unboundedQueue);
    CHECK(LbqDestroyLinkedBlockingQueue(&unboundedQueue) == NULL);
    return 0;
}
