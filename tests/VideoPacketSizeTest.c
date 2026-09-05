#include "Limelight-internal.h"
#include "SdpPacketSize.h"
#include "rs.h"

// Keep checks active in RelWithDebInfo, which is also used for local transport validation.
#undef assert
#define assert(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); } } while (0)

int AppVersionQuad[4] = {7, 1, 500, -1};
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
DECODER_RENDERER_CALLBACKS VideoCallbacks;
AUDIO_RENDERER_CALLBACKS AudioCallbacks;
struct sockaddr_storage RemoteAddr;
uint16_t RtspPortNumber = 48010;
uint16_t VideoPortNumber = 47998;
uint32_t EncryptionFeaturesSupported;
uint32_t EncryptionFeaturesRequested;
uint32_t EncryptionFeaturesEnabled;
bool AudioEncryptionEnabled;
bool HighQualitySurroundSupported;
bool HighQualitySurroundEnabled;
int AudioPacketDuration;
bool ReferenceFrameInvalidationSupported;
int NegotiatedVideoFormat;
volatile bool ConnectionInterrupted;
static int delivered;
static int expectedPayloadSize;
static unsigned char* expectedFrame;
static int expectedFrameSize;

// Platform.c supplies the real mutex/event primitives used by depacketizer teardown. These
// network lifecycle hooks must never run in this entirely in-process transport fixture.
int initializePlatformSockets(void) { assert(false); return -1; }
void cleanupPlatformSockets(void) { assert(false); }
void enterLowLatencyMode(void) { assert(false); }
void exitLowLatencyMode(void) { assert(false); }
int enet_initialize(void) { assert(false); return -1; }
void enet_deinitialize(void) { assert(false); }

uint64_t LiGetMillis(void) { return 100; }
bool LiGetCurrentHostDisplayHdrMode(void) { return false; }
bool isReferenceFrameInvalidationEnabled(void) { return false; }
bool isReferenceFrameInvalidationSupportedByDecoder(void) { return false; }
void LiRequestIdrFrame(void) {}
void notifyKeyFrameReceived(void) {}
void connectionReceivedCompleteFrame(uint32_t frameIndex) { assert(frameIndex == 1); }
void connectionDetectedFrameLoss(uint32_t first, uint32_t last) { (void)first; (void)last; assert(false); }
void connectionSawFrame(uint32_t frameIndex) { assert(frameIndex == 1); }
void connectionSendFrameFecStatus(PSS_FRAME_FEC_STATUS status) { (void)status; }
void addrToUrlSafeString(struct sockaddr_storage* addr, char* str, size_t size) {
    (void)addr;
    snprintf(str, size, "127.0.0.1");
}

// Exercise the production SDP writer: the announced size must be the same size later
// used by the receiver/FEC, and account for GCM before applying the optional host cap.
static void announcePacketSize(const char* describe, int networkBudget, bool encrypted,
                              bool hostForcesEncryption, int expected) {
    int maximum;
    assert(parseVideoPacketSizeMaximum(describe, &maximum));
    memset(&StreamConfig, 0, sizeof(StreamConfig));
    StreamConfig.packetSize = networkBudget;
    StreamConfig.width = 1920;
    StreamConfig.height = 1080;
    StreamConfig.fps = 60;
    StreamConfig.bitrate = 20000;
    StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    StreamConfig.streamingRemotely = STREAM_CFG_LOCAL;
    StreamConfig.encryptionFlags = encrypted && !hostForcesEncryption ? ENCFLG_VIDEO : 0;
    NegotiatedVideoFormat = VIDEO_FORMAT_H265;
    RemoteAddr.ss_family = AF_INET;
    EncryptionFeaturesSupported = encrypted ? SS_ENC_VIDEO : 0;
    EncryptionFeaturesRequested = hostForcesEncryption ? SS_ENC_VIDEO : 0;
    EncryptionFeaturesEnabled = 0;
    AudioEncryptionEnabled = false;
    int length;
    char* announce = getSdpPayloadForStreamConfig(13, &length, maximum);
    assert(announce && length > 0);
    char attribute[100];
    snprintf(attribute, sizeof(attribute), "a=x-nv-video[0].packetSize:%d \r\n", expected);
    assert(strstr(announce, attribute));
    assert(StreamConfig.packetSize == expected);
    assert(((EncryptionFeaturesEnabled & SS_ENC_VIDEO) != 0) == encrypted);
    free(announce);
}

static int submitDecodeUnit(PDECODE_UNIT unit) {
    assert(unit->fullLength == expectedFrameSize);
    int offset = 0;
    for (PLENTRY entry = unit->bufferList; entry; entry = entry->next) {
        assert(offset + entry->length <= expectedFrameSize);
        assert(memcmp(entry->data, expectedFrame + offset, entry->length) == 0);
        offset += entry->length;
    }
    assert(offset == expectedFrameSize);
    ++delivered;
    return DR_OK;
}

static void recoverPacket(int maximum, int lostIndex, int codec) {
    char sdp[100];
    snprintf(sdp, sizeof(sdp), "a=x-ss-video[0].maxPacketSize:%d\r\n", maximum);
    announcePacketSize(sdp, 1424, true, false, maximum);
    const int shardSize = StreamConfig.packetSize + MAX_RTP_HEADER_SIZE;
    expectedPayloadSize = StreamConfig.packetSize - sizeof(NV_VIDEO_PACKET);
    delivered = 0;
    NegotiatedVideoFormat = codec;
    StreamConfig.fps = 60;
    VideoCallbacks.capabilities = CAPABILITY_DIRECT_SUBMIT;
    VideoCallbacks.submitDecodeUnit = submitDecodeUnit;
    initializeVideoDepacketizer(StreamConfig.packetSize);
    const int padding = codec == VIDEO_FORMAT_AV1_MAIN8 ? 7 : 0;
    const int frameBytes = expectedPayloadSize * 4 - padding;
    unsigned char* framed = calloc(1, expectedPayloadSize * 4);
    framed[0] = 1;
    framed[3] = 2; // IDR
    framed[4] = (unsigned char)((expectedPayloadSize - padding) & 0xff);
    framed[5] = (unsigned char)((expectedPayloadSize - padding) >> 8);
    for (int i = 8; i < frameBytes; ++i) framed[i] = (unsigned char)(1 + i % 253);
    if (codec == VIDEO_FORMAT_H265) {
        // VPS, SPS, PPS and IDR framing exercises the real HEVC NAL splitter. Codec payload
        // contents are opaque here: the contract is exact bytes delivered to MediaCodec.
        const unsigned char headers[] = {
            0,0,0,1,64,1,128, 0,0,0,1,66,1,128, 0,0,0,1,68,1,128, 0,0,0,1,38,1,128
        };
        memcpy(framed + 8, headers, sizeof(headers));
    }
    expectedFrame = framed + 8;
    expectedFrameSize = frameBytes - 8;
    RTP_VIDEO_QUEUE queue;
    RtpvInitializeQueue(&queue);
    reed_solomon* rs = reed_solomon_new(4, 1);
    assert(rs);
    unsigned char* shards[5];
    for (int i = 0; i < 5; ++i) {
        shards[i] = calloc(1, RtpvQueueEntryOffset(shardSize) + sizeof(RTPV_QUEUE_ENTRY));
        assert(shards[i]);
        if (i < 4) {
            PNV_VIDEO_PACKET video = (PNV_VIDEO_PACKET)(shards[i] + MAX_RTP_HEADER_SIZE);
            video->streamPacketIndex = (unsigned int)i << 8;
            video->flags = FLAG_CONTAINS_PIC_DATA | (i == 0 ? FLAG_SOF : 0) | (i == 3 ? FLAG_EOF : 0);
            memcpy(video + 1, framed + i * expectedPayloadSize, expectedPayloadSize);
        }
    }
    assert(reed_solomon_encode(rs, shards, 5, shardSize) == 0);
    // Just like the host, initialize RTP and FEC routing fields after parity encoding.
    for (int i = 0; i < 5; ++i) {
        PRTP_PACKET rtp = (PRTP_PACKET)shards[i];
        rtp->header = 0x80 | FLAG_EXTENSION;
        rtp->sequenceNumber = BE16((uint16_t)i);
        rtp->timestamp = BE32(9000);
        PNV_VIDEO_PACKET video = (PNV_VIDEO_PACKET)(shards[i] + MAX_RTP_HEADER_SIZE);
        video->frameIndex = 1;
        video->multiFecBlocks = 0;
        video->fecInfo = ((unsigned int)i << 12) | (4U << 22) | (25U << 4);
    }
    // An interior loss is essential: zero-padding a recovered *last* fragment can be trimmed
    // by AV1/Annex-B, whereas padding this fragment changes the middle of the encoded frame.
    free(shards[lostIndex]);
    PPLT_CRYPTO_CONTEXT encrypt = PltCreateCryptoContext();
    PPLT_CRYPTO_CONTEXT decrypt = PltCreateCryptoContext();
    assert(encrypt && decrypt);
    for (int i = 0; i < 5; ++i) {
        if (i == lostIndex) continue;
        const int encryptedSize = shardSize + sizeof(ENC_VIDEO_HEADER);
        unsigned char* encrypted = calloc(1, encryptedSize);
        PENC_VIDEO_HEADER prefix = (PENC_VIDEO_HEADER)encrypted;
        prefix->iv[0] = (unsigned char)i;
        prefix->iv[11] = 'V';
        prefix->frameNumber = LE32(1);
        int encryptedBytes = 0;
        assert(PltEncryptMessage(encrypt, ALGORITHM_AES_GCM, 0,
                (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                prefix->iv, sizeof(prefix->iv), prefix->tag, sizeof(prefix->tag),
                shards[i], shardSize, encrypted + sizeof(*prefix), &encryptedBytes));
        assert(encryptedBytes + (int)sizeof(*prefix) == maximum + MAX_RTP_HEADER_SIZE + 32);
        free(shards[i]);
        unsigned char* clear = calloc(1, RtpvQueueEntryOffset(shardSize) + sizeof(RTPV_QUEUE_ENTRY));
        int clearBytes = 0;
        assert(PltDecryptMessage(decrypt, ALGORITHM_AES_GCM, 0,
                (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                prefix->iv, sizeof(prefix->iv), prefix->tag, sizeof(prefix->tag),
                encrypted + sizeof(*prefix), encryptedBytes, clear, &clearBytes));
        assert(clearBytes == shardSize);
        free(encrypted);
        PRTP_PACKET rtp = (PRTP_PACKET)clear;
        rtp->sequenceNumber = BE16(rtp->sequenceNumber);
        rtp->timestamp = BE32(rtp->timestamp);
        PRTPV_QUEUE_ENTRY entry = (PRTPV_QUEUE_ENTRY)(clear + RtpvQueueEntryOffset(shardSize));
        assert((uintptr_t)entry % _Alignof(RTPV_QUEUE_ENTRY) == 0);
        assert(RtpvAddPacket(&queue, rtp, clearBytes, entry) == RTPF_RET_QUEUED);
    }
    assert(delivered == 1);
    RtpvCleanupQueue(&queue);
    reed_solomon_release(rs);
    destroyVideoDepacketizer();
    PltDestroyCryptoContext(encrypt);
    PltDestroyCryptoContext(decrypt);
    free(framed);
}

int main(void) {
    int selected = 1392;
    assert(parseVideoPacketSizeMaximum("a=x-ss-general.featureFlags:0\r\n", &selected) && selected == 0);
    assert(parseVideoPacketSizeMaximum("a=x-ss-video[0].maxPacketSize:2000\n", &selected) && selected == 2000);
    const char* invalid[] = {"0", "199", "65460", "4294967297", "-1", "1346bad", "", "+1346"};
    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        char sdp[100];
        snprintf(sdp, sizeof(sdp), "a=x-ss-video[0].maxPacketSize:%s\r\n", invalid[i]);
        selected = 1392;
        assert(!parseVideoPacketSizeMaximum(sdp, &selected) && selected == 1392);
    }
    assert(!parseVideoPacketSizeMaximum("a=x-ss-video[0].maxPacketSize:1346\na=x-ss-video[0].maxPacketSize:1000\n", &selected));
    // Original Sunshine/Apollo omit the optional maximum: preserve their LAN/WAN sizes
    // exactly, both with client-selected encryption and host-forced encryption.
    const int budgets[] = {1392, 1024, 1184};
    for (unsigned int i = 0; i < sizeof(budgets) / sizeof(budgets[0]); ++i) {
        announcePacketSize("a=x-ss-general.featureFlags:0\r\n", budgets[i], false, false, budgets[i]);
        announcePacketSize("", budgets[i], true, false, budgets[i] - 32);
        announcePacketSize("", budgets[i], true, true, budgets[i] - 32);
        announcePacketSize("a=x-ss-video[0].maxPacketSize:2000\n", budgets[i], true, false, budgets[i] - 32);
    }
    announcePacketSize("a=x-ss-video[0].maxPacketSize:1346\n", 1392, true, true, 1346);
    announcePacketSize("a=x-ss-video[0].maxPacketSize:1346\n", 1024, true, false, 992);
    announcePacketSize("a=x-ss-video[0].maxPacketSize:200\n", 1024, true, false, 200);
    AppVersionQuad[3] = 0; // GFE also retains its original packet size with no extension.
    announcePacketSize("", 1392, false, false, 1392);
    AppVersionQuad[3] = -1;
    const int caps[] = {1346, 200, 1392};
    const int losses[] = {0, 1, 3};
    const int codecs[] = {VIDEO_FORMAT_H265, VIDEO_FORMAT_AV1_MAIN8};
    for (unsigned int c = 0; c < sizeof(caps) / sizeof(caps[0]); ++c)
        for (unsigned int l = 0; l < sizeof(losses) / sizeof(losses[0]); ++l)
            for (unsigned int v = 0; v < sizeof(codecs) / sizeof(codecs[0]); ++v)
                recoverPacket(caps[c], losses[l], codecs[v]);
    return 0;
}
