// Client-to-host Opus transport, compatible with the VoidLink microphone extension.
// Audio capture, resampling, and Opus encoding remain owned by the client.

#include "Limelight-internal.h"

#define MIC_IV_LEN 16

// This lock outlives individual connections so a capture callback racing teardown
// can safely observe a closed stream without touching a deleted mutex or context.
#if defined(LC_WINDOWS)
static PLT_MUTEX micMutex = SRWLOCK_INIT;
#elif !defined(__WIIU__) && !defined(__3DS__)
static PLT_MUTEX micMutex = PTHREAD_MUTEX_INITIALIZER;
#else
// These platforms have no static PLT_MUTEX initializer. Keep the same lifetime
// using their compiler's atomic primitives; socket sends below are nonblocking.
static volatile int micLock;
#endif

static void lockMicrophone(void) {
#if defined(__WIIU__) || defined(__3DS__)
    while (__sync_lock_test_and_set(&micLock, 1)) {
        PltSleepMs(1);
    }
#else
    PltLockMutex(&micMutex);
#endif
}

static void unlockMicrophone(void) {
#if defined(__WIIU__) || defined(__3DS__)
    __sync_lock_release(&micLock);
#else
    PltUnlockMutex(&micMutex);
#endif
}

static SOCKET micSocket = INVALID_SOCKET;
static PPLT_CRYPTO_CONTEXT micEncryptionCtx;
static PLT_THREAD micPingThread;
static bool micPingThreadStarted;
static bool micEncrypted;
static uint32_t micRiKeyId;
static uint16_t micSequenceNumber;
static unsigned char micKey[16];
static LC_SOCKADDR micAddress;
static SOCKADDR_LEN micAddressLength;
static SS_PING micPingPayload;

#pragma pack(push, 1)
typedef struct _MICROPHONE_PACKET_HEADER {
    uint8_t flags;
    uint8_t packetType;
    uint16_t sequenceNumber;
    uint32_t timestamp;
    uint32_t ssrc;
} MICROPHONE_PACKET_HEADER;
#pragma pack(pop)

static void microphonePingThreadProc(void* context) {
    uint32_t pingCount = 0;
    (void)context;

    // These values remain immutable until this thread is interrupted and joined.
    // The thread never acquires micMutex, allowing teardown to join while holding it.
    while (!PltIsThreadInterrupted(&micPingThread)) {
        if (micPingPayload.payload[0] != 0) {
            micPingPayload.sequenceNumber = BE32(++pingCount);
            sendto(micSocket, (const char*)&micPingPayload, sizeof(micPingPayload), 0,
                   (struct sockaddr*)&micAddress, micAddressLength);
        }
        else {
            sendto(micSocket, "PING", 4, 0, (struct sockaddr*)&micAddress, micAddressLength);
        }

        // Ignore transient ping errors, including hosts that have not bound yet.
        PltSleepMsInterruptible(&micPingThread, 500);
    }
}

static void destroyMicrophoneStreamLocked(void) {
    if (micPingThreadStarted) {
        PltInterruptThread(&micPingThread);
        PltJoinThread(&micPingThread);
        micPingThreadStarted = false;
    }
    if (micSocket != INVALID_SOCKET) {
        closeSocket(micSocket);
        micSocket = INVALID_SOCKET;
    }
    if (micEncryptionCtx != NULL) {
        PltDestroyCryptoContext(micEncryptionCtx);
        micEncryptionCtx = NULL;
    }
    micEncrypted = false;
    micRiKeyId = 0;
    micSequenceNumber = 0;
    memset(micKey, 0, sizeof(micKey));
    memset(&micAddress, 0, sizeof(micAddress));
    memset(&micPingPayload, 0, sizeof(micPingPayload));
    micAddressLength = 0;
}

int initializeMicrophoneStream(void) {
    int err = 0;
    lockMicrophone();

    if (micSocket != INVALID_SOCKET) {
        goto Exit;
    }
    if (ConnectionInterrupted || !StreamConfig.redirectMic || MicPortNumber == 0) {
        err = -1;
        goto Exit;
    }

    micEncrypted = (EncryptionFeaturesEnabled & SS_ENC_MICROPHONE) != 0;
    if (micEncrypted) {
        micEncryptionCtx = PltCreateCryptoContext();
        if (micEncryptionCtx == NULL) {
            err = -1;
            goto Cleanup;
        }
    }
    memcpy(micKey, StreamConfig.remoteInputAesKey, sizeof(micKey));
    memcpy(&micRiKeyId, StreamConfig.remoteInputAesIv, sizeof(micRiKeyId));
    micRiKeyId = BE32(micRiKeyId);
    micSequenceNumber = 0;
    memcpy(&micAddress, &RemoteAddr, sizeof(micAddress));
    SET_PORT(&micAddress, MicPortNumber);
    micAddressLength = AddrLen;
    micPingPayload = MicPingPayload;

    micSocket = bindUdpSocket(RemoteAddr.ss_family, &LocalAddr, AddrLen, 0, SOCK_QOS_TYPE_AUDIO);
    if (micSocket == INVALID_SOCKET) {
        err = LastSocketFail();
        goto Cleanup;
    }
    // A stalled socket must not hold capture or connection teardown indefinitely.
    if (setSocketNonBlocking(micSocket, true) != 0) {
        err = LastSocketFail();
        goto Cleanup;
    }
    err = PltCreateThread("MicPing", microphonePingThreadProc, NULL, &micPingThread);
    if (err != 0) {
        goto Cleanup;
    }
    micPingThreadStarted = true;
    goto Exit;

Cleanup:
    destroyMicrophoneStreamLocked();
Exit:
    unlockMicrophone();
    return err;
}

void destroyMicrophoneStream(void) {
    lockMicrophone();
    destroyMicrophoneStreamLocked();
    unlockMicrophone();
}

int sendMicrophoneOpusData(const unsigned char* opusData, int opusLength) {
    MICROPHONE_PACKET_HEADER header = {0};
    unsigned char packet[MAX_MIC_PACKET_SIZE];
    int payloadLength = opusLength;
    int ret = -1;

    if (opusData == NULL || opusLength <= 0 ||
            opusLength > MAX_MIC_PACKET_SIZE - (int)sizeof(header)) {
        return -1;
    }

    lockMicrophone();
    if (micSocket == INVALID_SOCKET) {
        goto Exit;
    }

    header.packetType = MIC_PACKET_TYPE_OPUS;
    header.sequenceNumber = LE16(micSequenceNumber);
    header.timestamp = LE32((uint32_t)PltGetMillis());
    header.ssrc = LE32(MIC_PACKET_MAGIC);
    memcpy(packet, &header, sizeof(header));

    if (micEncrypted) {
        unsigned char iv[MIC_IV_LEN] = {0};
        unsigned char plaintext[MAX_MIC_PACKET_SIZE];
        uint32_t ivSeq = BE32(micRiKeyId + micSequenceNumber);

        // The legacy crypto flags first pad the input to a block boundary,
        // then FINISH adds the cipher's PKCS7 block. Preserve that wire format
        // and reserve both before crypto writes. PAD_TO_BLOCK_SIZE mutates its
        // input, so it must never receive the capture caller's borrowed buffer.
        if (ROUND_TO_PKCS7_PADDED_LEN(opusLength) + 16 > MAX_MIC_PACKET_SIZE - (int)sizeof(header)) {
            goto Exit;
        }
        memcpy(plaintext, opusData, opusLength);
        memcpy(iv, &ivSeq, sizeof(ivSeq));
        if (!PltEncryptMessage(micEncryptionCtx, ALGORITHM_AES_CBC,
                CIPHER_FLAG_RESET_IV | CIPHER_FLAG_FINISH | CIPHER_FLAG_PAD_TO_BLOCK_SIZE,
                micKey, sizeof(micKey), iv, sizeof(iv), NULL, 0,
                plaintext, opusLength, packet + sizeof(header), &payloadLength)) {
            goto Exit;
        }
        if (payloadLength <= 0 || payloadLength > MAX_MIC_PACKET_SIZE - (int)sizeof(header)) {
            goto Exit;
        }
    }
    else {
        memcpy(packet + sizeof(header), opusData, opusLength);
    }

    // Consume the sequence even on send failure so encryption never reuses an IV
    // when a retry follows an uncertain socket result (apart from legacy wrap).
    micSequenceNumber++;
    ret = (int)sendto(micSocket, (const char*)packet, sizeof(header) + payloadLength, 0,
                      (struct sockaddr*)&micAddress, micAddressLength);
    if (ret < 0) {
        ret = -1;
    }

Exit:
    unlockMicrophone();
    return ret;
}

bool isMicrophoneEncryptionEnabled(void) {
    bool encrypted;
    lockMicrophone();
    encrypted = micSocket != INVALID_SOCKET && micEncrypted;
    unlockMicrophone();
    return encrypted;
}
