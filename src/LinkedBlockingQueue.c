#include "LinkedBlockingQueue.h"

// Destroy the linked blocking queue and associated mutex and event
PLINKED_BLOCKING_QUEUE_ENTRY LbqDestroyLinkedBlockingQueue(PLINKED_BLOCKING_QUEUE queueHead) {
    LC_ASSERT(queueHead->shutdown || queueHead->draining || queueHead->lifetimeSize == 0);
    
    PltDeleteMutex(&queueHead->mutex);
    PltDeleteConditionVariable(&queueHead->cond);

    return queueHead->head;
}

// Flush the queue
PLINKED_BLOCKING_QUEUE_ENTRY LbqFlushQueueItems(PLINKED_BLOCKING_QUEUE queueHead) {
    PLINKED_BLOCKING_QUEUE_ENTRY head;

    PltLockMutex(&queueHead->mutex);

    // Save the old head
    head = queueHead->head;

    // Reinitialize the queue to empty
    if (head != NULL) {
        queueHead->head = NULL;
        queueHead->tail = NULL;
        queueHead->currentSize = 0;
    }
    else {
        LC_ASSERT(queueHead->tail == NULL);
        LC_ASSERT(queueHead->currentSize == 0);
    }

    PltUnlockMutex(&queueHead->mutex);

    return head;
}

// Linked blocking queue init
int LbqInitializeLinkedBlockingQueue(PLINKED_BLOCKING_QUEUE queueHead, int sizeBound) {
    int err;

    memset(queueHead, 0, sizeof(*queueHead));

    err = PltCreateMutex(&queueHead->mutex);
    if (err != 0) {
        return err;
    }

    err = PltCreateConditionVariable(&queueHead->cond, &queueHead->mutex);
    if (err != 0) {
        PltDeleteMutex(&queueHead->mutex);
        return err;
    }

    queueHead->sizeBound = sizeBound;

    return 0;
}

void LbqSignalQueueShutdown(PLINKED_BLOCKING_QUEUE queueHead) {
    PltLockMutex(&queueHead->mutex);
    queueHead->shutdown = true;
    PltUnlockMutex(&queueHead->mutex);
    PltSignalConditionVariable(&queueHead->cond);
}

void LbqSignalQueueDrain(PLINKED_BLOCKING_QUEUE queueHead) {
    PltLockMutex(&queueHead->mutex);
    queueHead->draining = true;
    PltUnlockMutex(&queueHead->mutex);
    PltSignalConditionVariable(&queueHead->cond);
}

void LbqSignalQueueUserWake(PLINKED_BLOCKING_QUEUE queueHead) {
    PltLockMutex(&queueHead->mutex);
    queueHead->pendingUserWake = true;
    PltUnlockMutex(&queueHead->mutex);
    PltSignalConditionVariable(&queueHead->cond);
}

int LbqGetItemCount(PLINKED_BLOCKING_QUEUE queueHead) {
    int itemCount;

    // currentSize is written while holding this mutex. Reading it without the
    // same synchronization is a data race and can make input senders miss a
    // packet which is already queued behind the current packet.
    PltLockMutex(&queueHead->mutex);
    itemCount = queueHead->currentSize;
    PltUnlockMutex(&queueHead->mutex);

    return itemCount;
}

static int offerQueueItem(PLINKED_BLOCKING_QUEUE queueHead, void* data,
                          PLINKED_BLOCKING_QUEUE_ENTRY entry, bool evictHead,
                          void** evictedData) {
    PLINKED_BLOCKING_QUEUE_ENTRY evictedEntry = NULL;
    bool wasEmpty;

    entry->flink = NULL;
    entry->data = data;
    if (evictedData != NULL) {
        *evictedData = NULL;
    }

    PltLockMutex(&queueHead->mutex);

    if (queueHead->shutdown || queueHead->draining) {
        PltUnlockMutex(&queueHead->mutex);
        return LBQ_INTERRUPTED;
    }

    wasEmpty = queueHead->head == NULL;

    if (queueHead->sizeBound > 0 && queueHead->currentSize == queueHead->sizeBound) {
        if (!evictHead) {
            PltUnlockMutex(&queueHead->mutex);
            return LBQ_BOUND_EXCEEDED;
        }

        evictedEntry = queueHead->head;
        LC_ASSERT(evictedEntry != NULL);

        queueHead->head = evictedEntry->flink;
        queueHead->currentSize--;
        if (queueHead->head == NULL) {
            LC_ASSERT(queueHead->currentSize == 0);
            queueHead->tail = NULL;
        }
        else {
            queueHead->head->blink = NULL;
        }
    }

    if (queueHead->head == NULL) {
        LC_ASSERT(queueHead->currentSize == 0);
        LC_ASSERT(queueHead->tail == NULL);
        queueHead->head = entry;
        queueHead->tail = entry;
        entry->blink = NULL;
    }
    else {
        LC_ASSERT(queueHead->currentSize >= 1);
        LC_ASSERT(queueHead->tail != NULL);
        queueHead->tail->flink = entry;
        entry->blink = queueHead->tail;
        queueHead->tail = entry;
    }

    queueHead->currentSize++;
    queueHead->lifetimeSize++;
    if (evictedEntry != NULL && evictedData != NULL) {
        *evictedData = evictedEntry->data;
    }

    PltUnlockMutex(&queueHead->mutex);

    if (wasEmpty) {
        PltSignalConditionVariable(&queueHead->cond);
    }

    return LBQ_SUCCESS;
}

int LbqOfferQueueItem(PLINKED_BLOCKING_QUEUE queueHead, void* data, PLINKED_BLOCKING_QUEUE_ENTRY entry) {
    return offerQueueItem(queueHead, data, entry, false, NULL);
}

// Atomically append an item, evicting only the oldest item if the queue is full.
// The caller owns evictedData and must release it after this function returns.
int LbqOfferQueueItemWithHeadEviction(PLINKED_BLOCKING_QUEUE queueHead, void* data,
                                      PLINKED_BLOCKING_QUEUE_ENTRY entry, void** evictedData) {
    LC_ASSERT(evictedData != NULL);
    return offerQueueItem(queueHead, data, entry, true, evictedData);
}

// This must be synchronized with LbqFlushQueueItems by the caller
int LbqPeekQueueElement(PLINKED_BLOCKING_QUEUE queueHead, void** data) {
    PltLockMutex(&queueHead->mutex);

    if (queueHead->shutdown) {
        PltUnlockMutex(&queueHead->mutex);
        return LBQ_INTERRUPTED;
    }

    if (queueHead->head == NULL) {
        if (queueHead->draining) {
            PltUnlockMutex(&queueHead->mutex);
            return LBQ_INTERRUPTED;
        }
        else {
            PltUnlockMutex(&queueHead->mutex);
            return LBQ_NO_ELEMENT;
        }
    }

    *data = queueHead->head->data;

    PltUnlockMutex(&queueHead->mutex);

    return LBQ_SUCCESS;
}

int LbqPollQueueElement(PLINKED_BLOCKING_QUEUE queueHead, void** data) {
    PLINKED_BLOCKING_QUEUE_ENTRY entry;

    PltLockMutex(&queueHead->mutex);

    if (queueHead->shutdown) {
        PltUnlockMutex(&queueHead->mutex);
        return LBQ_INTERRUPTED;
    }

    if (queueHead->head == NULL) {
        if (queueHead->draining) {
            PltUnlockMutex(&queueHead->mutex);
            return LBQ_INTERRUPTED;
        }
        else {
            PltUnlockMutex(&queueHead->mutex);
            return LBQ_NO_ELEMENT;
        }
    }

    entry = queueHead->head;
    queueHead->head = entry->flink;
    queueHead->currentSize--;
    if (queueHead->head == NULL) {
        LC_ASSERT(queueHead->currentSize == 0);
        queueHead->tail = NULL;
    }
    else {
        LC_ASSERT(queueHead->currentSize != 0);
        queueHead->head->blink = NULL;
    }

    *data = entry->data;

    PltUnlockMutex(&queueHead->mutex);

    return LBQ_SUCCESS;
}

int LbqWaitForQueueElement(PLINKED_BLOCKING_QUEUE queueHead, void** data) {
    PLINKED_BLOCKING_QUEUE_ENTRY entry;

    PltLockMutex(&queueHead->mutex);

    // Wait for a waking condition: either data available or rundown
    while (queueHead->head == NULL && !queueHead->draining && !queueHead->shutdown && !queueHead->pendingUserWake) {
        PltWaitForConditionVariable(&queueHead->cond, &queueHead->mutex);
    }

    // If we're shutting down, abort immediately, even if there's data available
    if (queueHead->shutdown) {
        PltUnlockMutex(&queueHead->mutex);
        return LBQ_INTERRUPTED;
    }

    // If this is a user requested wake, process it now
    if (queueHead->pendingUserWake) {
        queueHead->pendingUserWake = false;
        PltUnlockMutex(&queueHead->mutex);
        return LBQ_USER_WAKE;
    }

    // If we're draining, only abort if we have no data available
    if (queueHead->draining && queueHead->head == NULL) {
        PltUnlockMutex(&queueHead->mutex);
        return LBQ_INTERRUPTED;
    }

    // We should have bailed by this point if there was no data
    LC_ASSERT(queueHead->head != NULL);

    entry = queueHead->head;
    queueHead->head = entry->flink;
    queueHead->currentSize--;
    if (queueHead->head == NULL) {
        LC_ASSERT(queueHead->currentSize == 0);
        queueHead->tail = NULL;
    }
    else {
        LC_ASSERT(queueHead->currentSize != 0);
        queueHead->head->blink = NULL;
    }

    *data = entry->data;

    PltUnlockMutex(&queueHead->mutex);

    return LBQ_SUCCESS;
}
