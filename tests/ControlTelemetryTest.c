// Include the production control queue so bounds, coalescing and subscription
// serialization are exercised without exporting a test API or opening a connection.
#define LC_CONTROL_TELEMETRY_TEST
#include "../src/ControlStream.c"

#undef assert
#define assert(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); } } while (0)

int AppVersionQuad[4] = {7, 1, 500, -1};
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
uint32_t SunshineFeatureFlags;
static int sends;
static uint8_t subscription[8];
static int modeSends;
static uint8_t modeRequest[20];
static int callbacks;
static uint8_t observed[3][HOST_SBS_TELEMETRY_STATE_SIZE];
static int depthCallbacks;
static int gameCallbacks;

// Platform lifecycle hooks must remain unused in this in-process fixture.
int initializePlatformSockets(void) { assert(false); return -1; }
void cleanupPlatformSockets(void) { assert(false); }
void enterLowLatencyMode(void) { assert(false); }
void exitLowLatencyMode(void) { assert(false); }
int enet_initialize(void) { assert(false); return -1; }
void enet_deinitialize(void) { assert(false); }

bool LiTestControlSend(short ptype, short paylen, const void* payload,
                       uint8_t channelId, uint32_t flags, bool moreData) {
    assert(ptype == 0x3009 || ptype == 0x3007);
    assert(channelId == CTRL_CHANNEL_SERVERCTL);
    assert(flags == ENET_PACKET_FLAG_RELIABLE);
    assert(!moreData);
    if (ptype == 0x3009) {
        assert(paylen == sizeof(subscription));
        memcpy(subscription, payload, sizeof(subscription));
        sends++;
    }
    else {
        assert(paylen == sizeof(modeRequest));
        memcpy(modeRequest, payload, sizeof(modeRequest));
        modeSends++;
    }
    return true;
}

static void receiveTelemetry(const uint8_t payload[HOST_SBS_TELEMETRY_STATE_SIZE]) {
    assert(callbacks < 3);
    memcpy(observed[callbacks++], payload, HOST_SBS_TELEMETRY_STATE_SIZE);
}

static void receiveDepthStatus(uint8_t phase) {
    assert(phase == 2);
    depthCallbacks++;
}

static void queueTelemetry(int bodyLength, uint8_t version, uint8_t marker) {
    size_t packetLength = sizeof(NVCTL_ENET_PACKET_HEADER_V1) + bodyLength;
    PNVCTL_ENET_PACKET_HEADER_V1 packet = calloc(1, packetLength);
    assert(packet);
    packet->type = packetTypes[IDX_HOST_SBS_TELEMETRY_STATE];
    if (bodyLength > 0) {
        uint8_t* body = (uint8_t*)(packet + 1);
        memset(body, marker, bodyLength);
        body[0] = version;
    }
    queueAsyncCallback(packet, (int)packetLength);
    free(packet);
}

static void testSubscription(void) {
    packetTypes = (short*)packetTypesGen7Enc;
    SunshineFeatureFlags = 0;
    assert(LiSendHostSbsTelemetrySubscription(true, true, 0xabcd, 100) == -1);
    assert(sends == 0); // Original Sunshine/Apollo never receive this extension.
    SunshineFeatureFlags = LI_FF_HOST_SBS_TELEMETRY_V2;
    assert(LiSendHostSbsTelemetrySubscription(true, true, 0xabcd, 0x1234) == 1);
    const uint8_t enabled[] = {2, 3, 0xcd, 0xab, 0x34, 0x12, 0, 0};
    assert(memcmp(subscription, enabled, sizeof(enabled)) == 0);
    assert(LiSendHostSbsTelemetrySubscription(false, false, 0x4321, 500) == 1);
    const uint8_t disabled[] = {2, 0, 0x21, 0x43, 0xf4, 1, 0, 0};
    assert(memcmp(subscription, disabled, sizeof(disabled)) == 0);
    packetTypes = (short*)packetTypesGen7;
    assert(LiSendHostSbsTelemetrySubscription(true, true, 1, 100) == -1);
    assert(sends == 2);
    packetTypes = (short*)packetTypesGen7Enc;
}

static void testBoundsAndCoalescing(void) {
    assert(LbqInitializeLinkedBlockingQueue(&asyncCallbackQueue, 16) == 0);
    ListenerCallbacks.hostSbsTelemetryState = receiveTelemetry;
    ListenerCallbacks.depthStatus = receiveDepthStatus;
    SunshineFeatureFlags = 0;
    queueTelemetry(240, 2, 1);
    assert(LbqGetItemCount(&asyncCallbackQueue) == 0);
    SunshineFeatureFlags = LI_FF_HOST_SBS_TELEMETRY_V2;
    const int invalidSizes[] = {0, 1, 88, 239, 241};
    for (size_t index = 0; index < sizeof(invalidSizes) / sizeof(invalidSizes[0]); index++) {
        queueTelemetry(invalidSizes[index], 2, 1);
    }
    queueTelemetry(240, 1, 1);
    queueTelemetry(240, 3, 1);
    assert(LbqGetItemCount(&asyncCallbackQueue) == 0);
    queueTelemetry(240, 2, 0x11);
    queueTelemetry(240, 2, 0x22);
    // Coalescing must not skip an intervening operational callback.
    uint8_t depthBytes[sizeof(NVCTL_ENET_PACKET_HEADER_V1) + 1];
    PNVCTL_ENET_PACKET_HEADER_V1 depth = (PNVCTL_ENET_PACKET_HEADER_V1)depthBytes;
    depth->type = packetTypes[IDX_DEPTH_STATUS];
    depthBytes[sizeof(*depth)] = 2;
    queueAsyncCallback(depth, sizeof(depthBytes));
    queueTelemetry(240, 2, 0x33);
    queueTelemetry(240, 2, 0x44);
    assert(LbqGetItemCount(&asyncCallbackQueue) == 5);
    LbqSignalQueueDrain(&asyncCallbackQueue);
    asyncCallbackThreadFunc(NULL);
    assert(callbacks == 2);
    assert(depthCallbacks == 1);
    for (int index = 0; index < HOST_SBS_TELEMETRY_STATE_SIZE; index++) {
        assert(observed[0][index] == (index == 0 ? 2 : 0x22));
        assert(observed[1][index] == (index == 0 ? 2 : 0x44));
    }
    assert(LbqDestroyLinkedBlockingQueue(&asyncCallbackQueue) == NULL);
}

static void testGameModeNegotiationAndWire(void) {
    packetTypes = (short*)packetTypesGen7Enc;
    const uint32_t unsupported[] = {0, LI_FF_GAME_PROVIDER_V1, LI_FF_ATOMIC_PRESENTATION_MODE_V2};
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
        SunshineFeatureFlags = unsupported[i];
        assert(LiSendSetVideoModeV2(SBS_MODE_GAME_MONO, 7, 1920, 1080, 6000, 20000) == -1);
        assert(LiSendSetVideoModeV2(SBS_MODE_GAME_SBS, 7, 1920, 1080, 6000, 20000) == -1);
    }
    assert(modeSends == 0);
    SunshineFeatureFlags = LI_FF_ATOMIC_PRESENTATION_MODE_V2;
    assert(LiSendSetVideoModeV2(SBS_MODE_AI, 7, 1920, 1080, 6000, 20000) == 1);
    SunshineFeatureFlags |= LI_FF_GAME_PROVIDER_V1;
    assert(LiSendSetVideoModeV2(SBS_MODE_GAME_MONO, 0x89abcdef, 1920, 1080, 6000, 20000) == 1);
    const uint8_t expected[] = {
        2, 2, 0, 0, 0xef, 0xcd, 0xab, 0x89, 0x80, 7, 0x38, 4,
        0x70, 0x17, 0, 0, 0x20, 0x4e, 0, 0,
    };
    assert(memcmp(modeRequest, expected, sizeof(expected)) == 0);
    assert(LiSendSetVideoModeV2(SBS_MODE_GAME_SBS, 9, 1920, 1080, 6000, 20000) == 1);
    assert(modeRequest[1] == 3 && modeRequest[4] == 9);
    assert(LiSendSetVideoModeV2(4, 1, 1920, 1080, 6000, 20000) == -1);
    assert(LiSendSetVideoModeV2(3, 1, 1919, 1080, 6000, 20000) == -1);
    assert(LiSendSetVideoModeV2(3, 1, 1920, 1080, 99, 20000) == -1);
    assert(LiSendSetVideoModeV2(3, 1, 1920, 1080, 6000, 0) == -1);
    packetTypes = (short*)packetTypesGen7;
    assert(LiSendSetVideoModeV2(3, 1, 1920, 1080, 6000, 20000) == -1);
    assert(modeSends == 3);
    packetTypes = (short*)packetTypesGen7Enc;
}

static void receiveGameSourceStatus(uint8_t state, uint8_t provider, uint32_t generation,
                                     uint32_t revision, uint16_t sourceWidth, uint16_t sourceHeight,
                                     uint16_t packedWidth, uint16_t packedHeight) {
    assert(state == GAME_SOURCE_READY && provider == GAME_PROVIDER_RESHADE);
    assert(generation == 0x10203040 && revision == 0x89abcdef);
    assert(sourceWidth == 1920 && sourceHeight == 1080 && packedWidth == 3840 && packedHeight == 1080);
    gameCallbacks++;
}

static void queueGameStatus(const uint8_t* payload, int length) {
    uint8_t bytes[sizeof(NVCTL_ENET_PACKET_HEADER_V1) + GAME_SOURCE_STATUS_SIZE + 1];
    PNVCTL_ENET_PACKET_HEADER_V1 packet = (PNVCTL_ENET_PACKET_HEADER_V1)bytes;
    packet->type = packetTypes[IDX_GAME_SOURCE_STATUS];
    memcpy(bytes + sizeof(*packet), payload, length);
    queueAsyncCallback(packet, sizeof(*packet) + length);
}

static void testGameSourceStatusValidation(void) {
    const uint8_t valid[GAME_SOURCE_STATUS_SIZE + 1] = {
        1, 1, 1, 0, 0x40, 0x30, 0x20, 0x10, 0xef, 0xcd, 0xab, 0x89,
        0x80, 7, 0x38, 4, 0, 15, 0x38, 4, 0,
    };
    assert(LbqInitializeLinkedBlockingQueue(&asyncCallbackQueue, 2) == 0);
    ListenerCallbacks.gameSourceStatus = receiveGameSourceStatus;
    const uint32_t unsupported[] = {0, LI_FF_GAME_PROVIDER_V1, LI_FF_ATOMIC_PRESENTATION_MODE_V2};
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
        SunshineFeatureFlags = unsupported[i];
        queueGameStatus(valid, 20);
    }
    assert(LbqGetItemCount(&asyncCallbackQueue) == 0);
    SunshineFeatureFlags = LI_FF_GAME_PROVIDER_V1 | LI_FF_ATOMIC_PRESENTATION_MODE_V2;
    const int invalidLengths[] = {0, 1, 19, 21};
    for (size_t i = 0; i < sizeof(invalidLengths) / sizeof(invalidLengths[0]); i++) {
        queueGameStatus(valid, invalidLengths[i]);
    }
    const struct { int offset; uint8_t value; } invalidFields[] = {
        {0, 0}, {0, 2}, {1, 3}, {2, 2}, {2, 0}, {3, 1}, {12, 0x81}, {16, 0x80}, {18, 0x36},
    };
    for (size_t i = 0; i < sizeof(invalidFields) / sizeof(invalidFields[0]); i++) {
        uint8_t invalid[GAME_SOURCE_STATUS_SIZE];
        memcpy(invalid, valid, sizeof(invalid));
        invalid[invalidFields[i].offset] = invalidFields[i].value;
        queueGameStatus(invalid, sizeof(invalid));
    }
    for (int offset = 4; offset <= 8; offset += 4) {
        uint8_t invalid[GAME_SOURCE_STATUS_SIZE];
        memcpy(invalid, valid, sizeof(invalid));
        memset(invalid + offset, 0, 4);
        queueGameStatus(invalid, sizeof(invalid));
    }
    assert(LbqGetItemCount(&asyncCallbackQueue) == 0);
    QUEUED_ASYNC_CALLBACK decoded;
    for (uint8_t state = GAME_SOURCE_WAITING; state <= GAME_SOURCE_UNSUPPORTED; state++) {
        uint8_t payload[GAME_SOURCE_STATUS_SIZE];
        memcpy(payload, valid, sizeof(payload));
        payload[1] = state;
        assert(decodeGameSourceStatus(payload, sizeof(payload), &decoded));
        assert(decoded.data.gameSourceStatus.state == state);
    }
    queueGameStatus(valid, 20);
    queueGameStatus(valid, 20);
    queueGameStatus(valid, 20); // A lost observation remains replaceable by the next heartbeat.
    assert(LbqGetItemCount(&asyncCallbackQueue) == 2);
    LbqSignalQueueDrain(&asyncCallbackQueue);
    asyncCallbackThreadFunc(NULL);
    assert(gameCallbacks == 2);
    assert(LbqDestroyLinkedBlockingQueue(&asyncCallbackQueue) == NULL);
}

int main(void) {
    assert(HOST_SBS_TELEMETRY_VERSION == 2);
    assert(HOST_SBS_TELEMETRY_STATE_SIZE == 240);
    assert(LI_FF_HOST_SBS_TELEMETRY_V2 == 0x40000000);
    assert(ML_FF_HOST_SBS_TELEMETRY_V2 == 0x04);
    testSubscription();
    testBoundsAndCoalescing();
    assert(LI_FF_GAME_PROVIDER_V1 == 0x02000000 && ML_FF_GAME_PROVIDER_V1 == 0x80);
    testGameModeNegotiationAndWire();
    testGameSourceStatusValidation();
    return 0;
}
