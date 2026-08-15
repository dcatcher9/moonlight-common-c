#include "LinkedBlockingQueue.h"

#include <assert.h>

typedef struct _TEST_QUEUE_ITEM {
    LINKED_BLOCKING_QUEUE_ENTRY entry;
    int value;
} TEST_QUEUE_ITEM;

static TEST_QUEUE_ITEM* pollItem(PLINKED_BLOCKING_QUEUE queue) {
    TEST_QUEUE_ITEM* item;
    assert(LbqPollQueueElement(queue, (void**)&item) == LBQ_SUCCESS);
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

    assert(LbqInitializeLinkedBlockingQueue(&queue, 3) == 0);
    assert(LbqOfferQueueItem(&queue, &items[0], &items[0].entry) == LBQ_SUCCESS);
    assert(LbqOfferQueueItem(&queue, &items[1], &items[1].entry) == LBQ_SUCCESS);
    assert(LbqOfferQueueItem(&queue, &items[2], &items[2].entry) == LBQ_SUCCESS);

    assert(LbqOfferQueueItemWithHeadEviction(&queue,
                                             &items[3],
                                             &items[3].entry,
                                             (void**)&evictedItem) == LBQ_SUCCESS);
    assert(evictedItem == &items[0]);
    assert(LbqGetItemCount(&queue) == 3);

    // A saturated audio queue must lose exactly the oldest packet while
    // preserving the order of the complete newest latency window.
    assert(pollItem(&queue)->value == 2);
    assert(pollItem(&queue)->value == 3);
    assert(pollItem(&queue)->value == 4);
    assert(LbqPollQueueElement(&queue, (void**)&evictedItem) == LBQ_NO_ELEMENT);

    LbqSignalQueueDrain(&queue);
    assert(LbqDestroyLinkedBlockingQueue(&queue) == NULL);

    assert(LbqInitializeLinkedBlockingQueue(&singleItemQueue, 1) == 0);
    assert(LbqOfferQueueItem(&singleItemQueue, &items[0], &items[0].entry) == LBQ_SUCCESS);
    assert(LbqOfferQueueItemWithHeadEviction(&singleItemQueue,
                                             &items[1],
                                             &items[1].entry,
                                             (void**)&evictedItem) == LBQ_SUCCESS);
    assert(evictedItem == &items[0]);
    assert(pollItem(&singleItemQueue)->value == 2);
    LbqSignalQueueDrain(&singleItemQueue);
    assert(LbqDestroyLinkedBlockingQueue(&singleItemQueue) == NULL);

    // A zero bound is the generic LBQ convention for an unbounded queue.
    assert(LbqInitializeLinkedBlockingQueue(&unboundedQueue, 0) == 0);
    assert(LbqOfferQueueItem(&unboundedQueue, &items[0], &items[0].entry) == LBQ_SUCCESS);
    assert(LbqOfferQueueItemWithHeadEviction(&unboundedQueue,
                                             &items[1],
                                             &items[1].entry,
                                             (void**)&evictedItem) == LBQ_SUCCESS);
    assert(evictedItem == NULL);
    assert(pollItem(&unboundedQueue)->value == 1);
    assert(pollItem(&unboundedQueue)->value == 2);
    LbqSignalQueueDrain(&unboundedQueue);
    assert(LbqDestroyLinkedBlockingQueue(&unboundedQueue) == NULL);
    return 0;
}
