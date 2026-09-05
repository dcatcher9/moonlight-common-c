#pragma once

#include "Video.h"

typedef struct _RTPV_QUEUE_ENTRY {
    struct _RTPV_QUEUE_ENTRY* next;
    struct _RTPV_QUEUE_ENTRY* prev;
    PRTP_PACKET packet;
    uint64_t receiveTimeMs;
    uint32_t presentationTimeMs;
    int length;
    bool isParity;
} RTPV_QUEUE_ENTRY, *PRTPV_QUEUE_ENTRY;

// Packet payload sizes need not be aligned (for example a host maximum of 1346 bytes).
// Keep the queue metadata aligned without adding padding to the authenticated/FEC payload.
static inline int RtpvQueueEntryOffset(int packetSize) {
#ifdef __cplusplus
    const int alignment = alignof(RTPV_QUEUE_ENTRY);
#elif defined(_MSC_VER)
    const int alignment = __alignof(RTPV_QUEUE_ENTRY);
#else
    const int alignment = _Alignof(RTPV_QUEUE_ENTRY);
#endif
    return (packetSize + alignment - 1) / alignment * alignment;
}

typedef struct _RTPV_QUEUE_LIST {
    PRTPV_QUEUE_ENTRY head;
    PRTPV_QUEUE_ENTRY tail;
    uint32_t count;
} RTPV_QUEUE_LIST, *PRTPV_QUEUE_LIST;

typedef struct _RTP_VIDEO_QUEUE {
    RTPV_QUEUE_LIST pendingFecBlockList;
    RTPV_QUEUE_LIST completedFecBlockList;

    uint64_t bufferFirstRecvTimeMs;
    uint32_t bufferLowestSequenceNumber;
    uint32_t bufferHighestSequenceNumber;
    uint32_t bufferFirstParitySequenceNumber;
    uint32_t bufferDataPackets;
    uint32_t bufferParityPackets;
    uint32_t receivedDataPackets;
    uint32_t receivedParityPackets;
    uint32_t receivedHighestSequenceNumber;
    uint32_t fecPercentage;
    uint32_t nextContiguousSequenceNumber;
    uint32_t missingPackets; // # of holes behind receivedHighestSequenceNumber
    bool useFastQueuePath;
    bool reportedLostFrame;

    uint32_t currentFrameNumber;

    bool multiFecCapable;
    uint8_t multiFecCurrentBlockNumber;
    uint8_t multiFecLastBlockNumber;

    uint32_t lastOosFramePresentationTimestamp;
    bool receivedOosData;
} RTP_VIDEO_QUEUE, *PRTP_VIDEO_QUEUE;

#define RTPF_RET_QUEUED    0
#define RTPF_RET_REJECTED  1

void RtpvInitializeQueue(PRTP_VIDEO_QUEUE queue);
void RtpvCleanupQueue(PRTP_VIDEO_QUEUE queue);
int RtpvAddPacket(PRTP_VIDEO_QUEUE queue, PRTP_PACKET packet, int length, PRTPV_QUEUE_ENTRY packetEntry);
uint32_t RtpvGetCurrentFrameNumber(PRTP_VIDEO_QUEUE queue);
void RtpvSubmitQueuedPackets(PRTP_VIDEO_QUEUE queue);
