#include "Limelight-internal.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); abort(); } } while (0)

static int captureSend(SOCKET socket, const char* bytes, int size, int flags,
                       const struct sockaddr* address, SOCKADDR_LEN addressLength);
static PPLT_CRYPTO_CONTEXT createCrypto(void);
static void destroyCrypto(PPLT_CRYPTO_CONTEXT ctx);
static int createThread(const char* name, ThreadEntry entry, void* context, PLT_THREAD* thread);

// Production transport and teardown, real crypto and platform threading. Socket
// operations are intercepted to inspect exact datagrams and force failure paths.
#define sendto captureSend
#define PltCreateCryptoContext createCrypto
#define PltDestroyCryptoContext destroyCrypto
#define PltCreateThread createThread
#include "../src/MicrophoneStream.c"
#undef sendto
#undef PltCreateCryptoContext
#undef PltDestroyCryptoContext
#undef PltCreateThread

STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
struct sockaddr_storage RemoteAddr;
struct sockaddr_storage LocalAddr;
SOCKADDR_LEN AddrLen;
uint16_t MicPortNumber;
SS_PING MicPingPayload;
uint32_t EncryptionFeaturesEnabled;
volatile bool ConnectionInterrupted;

static PLT_MUTEX captureMutex;
static PLT_EVENT pingReceived;
static PLT_EVENT sendEntered;
static PLT_EVENT releaseSend;
static PLT_EVENT destroyEntered;
static unsigned char lastPacket[MAX_MIC_PACKET_SIZE];
static int lastPacketLength;
static unsigned char lastPing[sizeof(SS_PING)];
static int lastPingLength;
static int openSockets;
static int liveCrypto;
static int sentData;
static uint16_t expectedPort;
static bool failBind;
static bool failNonBlocking;
static bool failCrypto;
static bool failThread;
static bool failSend;
static bool blockSend;
static const unsigned char sample[] = {0xf8, 0x55, 0x31, 0xa4, 0x01};

int initializePlatformSockets(void) { return 0; }
void cleanupPlatformSockets(void) {}
void enterLowLatencyMode(void) {}
void exitLowLatencyMode(void) {}
int enet_initialize(void) { return 0; }
void enet_deinitialize(void) {}

SOCKET bindUdpSocket(int family, struct sockaddr_storage* local, SOCKADDR_LEN length, int buffer, int qos) {
    CHECK(family == AF_INET && local == &LocalAddr && length == AddrLen);
    CHECK(buffer == 0 && qos == SOCK_QOS_TYPE_AUDIO);
    if (failBind) { SetLastSocketError(ECONNREFUSED); return INVALID_SOCKET; }
    CHECK(openSockets == 0);
    openSockets++;
    return (SOCKET)123;
}

int setSocketNonBlocking(SOCKET socket, bool enabled) {
    CHECK(socket == (SOCKET)123 && enabled);
    if (failNonBlocking) { SetLastSocketError(ECONNREFUSED); return -1; }
    return 0;
}

void closeSocket(SOCKET socket) {
    CHECK(socket == (SOCKET)123 && openSockets == 1);
    // The real ping worker must have been joined before the socket is closed.
    CHECK(!micPingThreadStarted);
    openSockets--;
}

static PPLT_CRYPTO_CONTEXT createCrypto(void) {
    if (failCrypto) return NULL;
    PPLT_CRYPTO_CONTEXT ctx = PltCreateCryptoContext();
    CHECK(ctx != NULL);
    liveCrypto++;
    return ctx;
}

static void destroyCrypto(PPLT_CRYPTO_CONTEXT ctx) {
    CHECK(ctx != NULL && liveCrypto == 1);
    liveCrypto--;
    PltDestroyCryptoContext(ctx);
}

static int createThread(const char* name, ThreadEntry entry, void* context, PLT_THREAD* thread) {
    CHECK(strcmp(name, "MicPing") == 0);
    if (failThread) return -77;
    return PltCreateThread(name, entry, context, thread);
}

static int captureSend(SOCKET socket, const char* bytes, int size, int flags,
                       const struct sockaddr* address, SOCKADDR_LEN addressLength) {
    CHECK(socket == (SOCKET)123 && openSockets == 1 && flags == 0);
    CHECK(addressLength == AddrLen);
    CHECK(ntohs(((const struct sockaddr_in*)address)->sin_port) == expectedPort);
    CHECK(size > 0 && size <= MAX_MIC_PACKET_SIZE);

    if ((unsigned char)bytes[0] == 0 && size >= 12 && (unsigned char)bytes[1] == MIC_PACKET_TYPE_OPUS) {
        if (blockSend) {
            PltSetEvent(&sendEntered);
            PltWaitForEvent(&releaseSend);
        }
        PltLockMutex(&captureMutex);
        memcpy(lastPacket, bytes, size);
        lastPacketLength = size;
        sentData++;
        PltUnlockMutex(&captureMutex);
        if (failSend) { SetLastSocketError(ECONNREFUSED); return -1; }
    }
    else {
        CHECK(size == 4 || size == sizeof(SS_PING));
        PltLockMutex(&captureMutex);
        memcpy(lastPing, bytes, size);
        lastPingLength = size;
        PltUnlockMutex(&captureMutex);
        PltSetEvent(&pingReceived);
    }
    return size;
}

static void resetStream(bool encrypted, bool sessionPing) {
    destroyMicrophoneStream();
    CHECK(openSockets == 0 && liveCrypto == 0);
    failBind = failNonBlocking = failCrypto = failThread = failSend = blockSend = false;
    ConnectionInterrupted = false;
    memset(&StreamConfig, 0, sizeof(StreamConfig));
    StreamConfig.redirectMic = true;
    for (int i = 0; i < 16; i++) StreamConfig.remoteInputAesKey[i] = (char)i;
    StreamConfig.remoteInputAesIv[0] = 0x12;
    StreamConfig.remoteInputAesIv[1] = 0x34;
    StreamConfig.remoteInputAesIv[2] = 0x56;
    StreamConfig.remoteInputAesIv[3] = 0x78;
    EncryptionFeaturesEnabled = encrypted ? SS_ENC_MICROPHONE : 0;
    memset(&RemoteAddr, 0, sizeof(RemoteAddr));
    RemoteAddr.ss_family = AF_INET;
    AddrLen = sizeof(struct sockaddr_in);
    MicPortNumber = expectedPort = 47996;
    memset(&MicPingPayload, 0, sizeof(MicPingPayload));
    if (sessionPing) memcpy(MicPingPayload.payload, "123456789abcdef0", 16);
    lastPacketLength = lastPingLength = sentData = 0;
    PltClearEvent(&pingReceived);
    PltClearEvent(&sendEntered);
    PltClearEvent(&releaseSend);
    PltClearEvent(&destroyEntered);
}

static void checkHeader(uint16_t sequence) {
    MICROPHONE_PACKET_HEADER header;
    memcpy(&header, lastPacket, sizeof(header));
    CHECK(header.flags == 0 && header.packetType == 0x61);
    CHECK(LE16(header.sequenceNumber) == sequence);
    CHECK(LE32(header.ssrc) == UINT32_C(0x12345678));
    CHECK((uint32_t)PltGetMillis() - LE32(header.timestamp) < 2000);
}

static void checkEncrypted(const unsigned char* original, int size, uint16_t sequence) {
    unsigned char plaintext[MAX_MIC_PACKET_SIZE];
    unsigned char expected[MAX_MIC_PACKET_SIZE];
    unsigned char decrypted[MAX_MIC_PACKET_SIZE];
    unsigned char iv[16] = {0};
    uint32_t ivSeq = BE32(UINT32_C(0x12345678) + sequence);
    memcpy(iv, &ivSeq, sizeof(ivSeq));
    memcpy(plaintext, original, size);
    PPLT_CRYPTO_CONTEXT enc = PltCreateCryptoContext();
    PPLT_CRYPTO_CONTEXT dec = PltCreateCryptoContext();
    int expectedLength = 0;
    int decryptedLength = 0;
    CHECK(PltEncryptMessage(enc, ALGORITHM_AES_CBC,
        CIPHER_FLAG_RESET_IV | CIPHER_FLAG_FINISH | CIPHER_FLAG_PAD_TO_BLOCK_SIZE,
        (unsigned char*)StreamConfig.remoteInputAesKey, 16, iv, 16, NULL, 0,
        plaintext, size, expected, &expectedLength));
    CHECK(lastPacketLength == 12 + expectedLength);
    CHECK(memcmp(lastPacket + 12, expected, expectedLength) == 0);
    CHECK(PltDecryptMessage(dec, ALGORITHM_AES_CBC, CIPHER_FLAG_RESET_IV | CIPHER_FLAG_FINISH,
        (unsigned char*)StreamConfig.remoteInputAesKey, 16, iv, 16, NULL, 0,
        lastPacket + 12, lastPacketLength - 12, decrypted, &decryptedLength));
    CHECK(decryptedLength == ROUND_TO_PKCS7_PADDED_LEN(size));
    CHECK(memcmp(decrypted, original, size) == 0);
    PltDestroyCryptoContext(enc);
    PltDestroyCryptoContext(dec);
}

static void testPackets(void) {
    unsigned char payload[1400];
    memset(payload, 0x55, sizeof(payload));
    for (int encrypted = 0; encrypted < 2; encrypted++) {
        resetStream(encrypted != 0, encrypted != 0);
        CHECK(initializeMicrophoneStream() == 0);
        CHECK(initializeMicrophoneStream() == 0); // Idempotent, no second socket/thread.
        CHECK(isMicrophoneEncryptionEnabled() == (encrypted != 0));
        PltWaitForEvent(&pingReceived);
        PltLockMutex(&captureMutex);
        if (encrypted) {
            CHECK(lastPingLength == sizeof(SS_PING));
            CHECK(memcmp(lastPing, "123456789abcdef0", 16) == 0);
            CHECK(lastPing[16] == 0 && lastPing[17] == 0 && lastPing[18] == 0 && lastPing[19] == 1);
        }
        else CHECK(lastPingLength == 4 && memcmp(lastPing, "PING", 4) == 0);
        PltUnlockMutex(&captureMutex);

        // A truly const borrowed buffer catches the reference's in-place padding bug.
        CHECK(sendMicrophoneOpusData(sample, sizeof(sample)) > 0);
        checkHeader(0);
        if (encrypted) checkEncrypted(sample, sizeof(sample), 0);
        else CHECK(lastPacketLength == 12 + sizeof(sample) && memcmp(lastPacket + 12, sample, sizeof(sample)) == 0);
        int maximum = encrypted ? 1360 : 1388;
        CHECK(sendMicrophoneOpusData(payload, maximum) > 0);
        checkHeader(1);
        if (encrypted) checkEncrypted(payload, maximum, 1);
        for (size_t i = 0; i < sizeof(payload); i++) CHECK(payload[i] == 0x55);
        CHECK(sendMicrophoneOpusData(payload, maximum + 1) == -1);
        CHECK(sendMicrophoneOpusData(NULL, 1) == -1);
        CHECK(sendMicrophoneOpusData(payload, 0) == -1);
        CHECK(sendMicrophoneOpusData(payload, -1) == -1);
        CHECK(sendMicrophoneOpusData(payload, INT32_MAX) == -1);
        CHECK(sentData == 2);
        failSend = true;
        CHECK(sendMicrophoneOpusData(sample, sizeof(sample)) == -1);
        checkHeader(2);
        failSend = false;
        CHECK(sendMicrophoneOpusData(sample, sizeof(sample)) > 0);
        checkHeader(3);
        if (encrypted) checkEncrypted(sample, sizeof(sample), 3);
        micSequenceNumber = UINT16_MAX;
        CHECK(sendMicrophoneOpusData(sample, sizeof(sample)) > 0);
        checkHeader(UINT16_MAX);
        CHECK(sendMicrophoneOpusData(sample, sizeof(sample)) > 0);
        checkHeader(0);
        destroyMicrophoneStream();
        CHECK(!isMicrophoneEncryptionEnabled() && openSockets == 0 && liveCrypto == 0);
        CHECK(sendMicrophoneOpusData(sample, sizeof(sample)) == -1);
    }
    puts("PASS: legacy datagrams, ping formats, CBC ciphertext, const input, boundaries and sequence wrap");
}

static void testFailures(void) {
    resetStream(true, false);
    StreamConfig.redirectMic = false;
    CHECK(initializeMicrophoneStream() != 0 && openSockets == 0 && liveCrypto == 0);
    StreamConfig.redirectMic = true;
    MicPortNumber = 0;
    CHECK(initializeMicrophoneStream() != 0 && openSockets == 0 && liveCrypto == 0);
    MicPortNumber = 47996;
    ConnectionInterrupted = true;
    CHECK(initializeMicrophoneStream() != 0 && openSockets == 0 && liveCrypto == 0);
    ConnectionInterrupted = false;
    bool* failures[] = {&failCrypto, &failBind, &failNonBlocking, &failThread};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        *failures[i] = true;
        CHECK(initializeMicrophoneStream() != 0);
        CHECK(openSockets == 0 && liveCrypto == 0 && !micPingThreadStarted);
        CHECK(!isMicrophoneEncryptionEnabled());
        destroyMicrophoneStream();
        *failures[i] = false;
        CHECK(initializeMicrophoneStream() == 0);
        CHECK(sendMicrophoneOpusData(sample, sizeof(sample)) > 0);
        checkHeader(0);
        destroyMicrophoneStream();
    }
    puts("PASS: disabled/unsupported/cancelled starts and each partial initialization failure clean up");
}

static void sendThreadProc(void* context) {
    (void)context;
    CHECK(sendMicrophoneOpusData(sample, sizeof(sample)) > 0);
}

static void destroyThreadProc(void* context) {
    (void)context;
    PltSetEvent(&destroyEntered);
    destroyMicrophoneStream();
}

static void testConcurrentStop(void) {
    PLT_THREAD sender, stopper;
    resetStream(true, true);
    CHECK(initializeMicrophoneStream() == 0);
    blockSend = true;
    CHECK(PltCreateThread("MicTestSend", sendThreadProc, NULL, &sender) == 0);
    PltWaitForEvent(&sendEntered);
    CHECK(PltCreateThread("MicTestStop", destroyThreadProc, NULL, &stopper) == 0);
    PltWaitForEvent(&destroyEntered);
    CHECK(openSockets == 1 && liveCrypto == 1);
    PltSetEvent(&releaseSend);
    PltJoinThread(&sender);
    PltJoinThread(&stopper);
    CHECK(openSockets == 0 && liveCrypto == 0);
    CHECK(sendMicrophoneOpusData(sample, sizeof(sample)) == -1);
    resetStream(false, false);
    MicPortNumber = expectedPort = 51000;
    CHECK(initializeMicrophoneStream() == 0);
    CHECK(sendMicrophoneOpusData(sample, sizeof(sample)) > 0);
    checkHeader(0);
    destroyMicrophoneStream();
    puts("PASS: capture racing teardown completes before socket/context disposal; reconnect resets state");
}

int main(void) {
    CHECK(PltCreateMutex(&captureMutex) == 0);
    CHECK(PltCreateEvent(&pingReceived) == 0);
    CHECK(PltCreateEvent(&sendEntered) == 0);
    CHECK(PltCreateEvent(&releaseSend) == 0);
    CHECK(PltCreateEvent(&destroyEntered) == 0);
    testPackets();
    testFailures();
    testConcurrentStop();
    PltCloseEvent(&destroyEntered);
    PltCloseEvent(&releaseSend);
    PltCloseEvent(&sendEntered);
    PltCloseEvent(&pingReceived);
    PltDeleteMutex(&captureMutex);
    puts("Microphone stream tests passed (real crypto and threads, controlled UDP socket).");
    return 0;
}
