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
static int callbacks;
static uint8_t observed[3][HOST_SBS_TELEMETRY_STATE_SIZE];
static int depthCallbacks;

// Platform lifecycle hooks must remain unused in this in-process fixture.
int initializePlatformSockets(void) { assert(false); return -1; }
void cleanupPlatformSockets(void) { assert(false); }
void enterLowLatencyMode(void) { assert(false); }
void exitLowLatencyMode(void) { assert(false); }
int enet_initialize(void) { assert(false); return -1; }
void enet_deinitialize(void) { assert(false); }

bool LiTestControlSend(short ptype, short paylen, const void* payload,
                       uint8_t channelId, uint32_t flags, bool moreData) {
    assert(ptype == 0x3009);
    assert(paylen == sizeof(subscription));
    assert(channelId == CTRL_CHANNEL_SERVERCTL);
    assert(flags == ENET_PACKET_FLAG_RELIABLE);
    assert(!moreData);
    memcpy(subscription, payload, sizeof(subscription));
    sends++;
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

int main(void) {
    assert(HOST_SBS_TELEMETRY_VERSION == 2);
    assert(HOST_SBS_TELEMETRY_STATE_SIZE == 240);
    assert(LI_FF_HOST_SBS_TELEMETRY_V2 == 0x40000000);
    assert(ML_FF_HOST_SBS_TELEMETRY_V2 == 0x04);
    testSubscription();
    testBoundsAndCoalescing();
    return 0;
}
