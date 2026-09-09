// Exercise production parsers and control routing without sockets or a renderer.
// LTO discards unrelated control transport code, as in ControlTelemetryTest.
#include "../src/ControlStream.c"
#include <math.h>

#undef assert
#define assert(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); } } while (0)

int AppVersionQuad[4] = {7, 1, 500, -1};
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
uint32_t SunshineFeatureFlags;

int initializePlatformSockets(void) { assert(false); return -1; }
void cleanupPlatformSockets(void) { assert(false); }
void enterLowLatencyMode(void) { assert(false); }
void exitLowLatencyMode(void) { assert(false); }
int enet_initialize(void) { assert(false); return -1; }
void enet_deinitialize(void) { assert(false); }

static int pcmCalls, irCalls, standardCalls;
static LI_DS5_HAPTICS_PCM_FRAME copiedPcm;
static LI_DS5_HAPTICS_IR_FRAME_V2 copiedIr;
static uint8_t ownedPcm[DS5_HAPTICS_STREAM_MAX_FRAMES * 4];
static const uint8_t* expectedBorrowedPcm;
static int callbackContext;

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static void put64(uint8_t* p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static void putFloat(uint8_t* p, float v) { uint32_t bits; memcpy(&bits, &v, sizeof(bits)); put32(p, bits); }

static int makePcm(uint8_t* p, uint16_t frames, uint16_t controller, uint8_t flags) {
    int length = DS5_HAPTICS_STREAM_WIRE_HEADER_SIZE + frames * 4;
    memset(p, 0, length);
    p[0] = 1;
    p[1] = flags;
    put16(p + 2, 28);
    put16(p + 4, controller);
    put16(p + 6, frames);
    put32(p + 8, UINT32_C(0xfedcba98));
    put64(p + 12, UINT64_C(0xfedcba9876543210));
    put32(p + 20, 48000);
    p[24] = 2;
    p[25] = 16;
    // Signed extrema and distinct left/right samples must remain exact bytes.
    for (int i = 28; i < length; ++i) p[i] = (uint8_t)(i * 71);
    if (frames > 0) { put16(p + 28, 0x8000); put16(p + 30, 0x7fff); }
    return length;
}

static void makeIr(uint8_t* p) {
    memset(p, 0, 72);
    p[0] = 2;
    p[1] = 0x0f;
    put16(p + 2, 72);
    put16(p + 4, 15);
    put32(p + 8, UINT32_C(0x89abcdef));
    put64(p + 12, UINT64_C(0xfedcba9876543210));
    put32(p + 20, 480);
    for (int lane = 0; lane < 2; ++lane) {
        uint8_t* data = p + 24 + lane * 20;
        putFloat(data, 0.25f);
        putFloat(data + 4, 0.75f);
        putFloat(data + 8, 0.5f);
        putFloat(data + 12, 1.0f);
        putFloat(data + 16, 48000.0f);
    }
    putFloat(p + 64, -0.5f);
}

static void receivePcm(const LI_DS5_HAPTICS_PCM_FRAME* frame) {
    assert(frame->pcmData == expectedBorrowedPcm);
    assert(frame->pcmDataLength <= sizeof(ownedPcm));
    copiedPcm = *frame;
    memcpy(ownedPcm, frame->pcmData, frame->pcmDataLength);
    copiedPcm.pcmData = ownedPcm; // A renderer must own bytes retained past this callback.
    ++pcmCalls;
}

static void receiveIr(const LI_DS5_HAPTICS_IR_FRAME_V2* frame) { copiedIr = *frame; ++irCalls; }
static void parsePcmCallback(const LI_DS5_HAPTICS_PCM_FRAME* frame, void* context) {
    assert(context == &callbackContext);
    receivePcm(frame);
}
static void parseIrCallback(const LI_DS5_HAPTICS_IR_FRAME_V2* frame, void* context) {
    assert(context == &callbackContext);
    receiveIr(frame);
}
static bool parsePcm(const uint8_t* p, int length) {
    return processDs5HapticsStreamPacket(p, length, parsePcmCallback, &callbackContext);
}
static bool parseIr(const uint8_t* p, int length) {
    return processDs5HapticsIrStreamPacket(p, length, parseIrCallback, &callbackContext);
}

static void testPcmBoundsAndOwnership(void) {
    uint8_t payload[28 + 481 * 4 + 8], changed[sizeof(payload)];
    int length = makePcm(payload, 480, 15, 7);
    expectedBorrowedPcm = payload + 28;
    int before = pcmCalls;
    assert(parsePcm(payload, length));
    assert(pcmCalls == before + 1);
    assert(copiedPcm.controllerNumber == 15 && copiedPcm.flags == 7);
    assert(copiedPcm.frameCount == 480 && copiedPcm.pcmDataLength == 1920);
    assert(copiedPcm.sampleRate == 48000 && copiedPcm.channelCount == 2 && copiedPcm.bitsPerSample == 16);
    assert(copiedPcm.sequenceNumber == UINT32_C(0xfedcba98));
    assert(copiedPcm.presentationTimeUs == UINT64_C(0xfedcba9876543210));
    assert(memcmp(copiedPcm.pcmData, payload + 28, 1920) == 0);
    memset(payload, 0, sizeof(payload));
    assert(copiedPcm.pcmData[0] == 0 && copiedPcm.pcmData[1] == 0x80);
    assert(copiedPcm.pcmData[2] == 0xff && copiedPcm.pcmData[3] == 0x7f);
    assert(copiedPcm.controllerNumber == 15);

    // Empty end markers and sequence wrap remain meaningful to the consumer.
    length = makePcm(payload, 0, 0, LI_DS5_HAPTICS_PCM_FLAG_STREAM_END);
    put32(payload + 8, UINT32_MAX);
    put64(payload + 12, 0);
    expectedBorrowedPcm = payload + 28;
    assert(parsePcm(payload, length));
    assert(copiedPcm.frameCount == 0 && copiedPcm.pcmDataLength == 0);
    assert(copiedPcm.flags == LI_DS5_HAPTICS_PCM_FLAG_STREAM_END);
    assert(copiedPcm.sequenceNumber == UINT32_MAX && copiedPcm.presentationTimeUs == 0);
    length = makePcm(payload, 1, 0, LI_DS5_HAPTICS_PCM_FLAG_STREAM_START);
    put32(payload + 8, 0);
    assert(parsePcm(payload, length) && copiedPcm.sequenceNumber == 0);

    // Header extensions are skipped according to headerSize, never treated as PCM.
    memmove(payload + 32, payload + 28, 4);
    memset(payload + 28, 0xaa, 4);
    put16(payload + 2, 32);
    expectedBorrowedPcm = payload + 32;
    assert(parsePcm(payload, length + 4));
    assert(copiedPcm.pcmDataLength == 4 && copiedPcm.pcmData[1] == 0x80);

    length = makePcm(payload, 1, 0, 0);
    before = pcmCalls;
    assert(!parsePcm(NULL, length));
    assert(!processDs5HapticsStreamPacket(payload, length, NULL, &callbackContext));
    const int badLengths[] = {-1, 0, 1, 27, 28, 31, 33, UINT16_MAX + 1};
    for (size_t i = 0; i < sizeof(badLengths) / sizeof(badLengths[0]); ++i)
        assert(!parsePcm(payload, badLengths[i]));
    const struct { int offset; uint8_t value; } invalid[] = {
        {0, 0}, {0, 2}, {1, 8}, {1, 0x80}, {2, 27}, {2, 33},
        {3, 1}, {4, 16}, {5, 0xff}, {6, 0}, {6, 2},
        {20, 0}, {24, 1}, {24, 3}, {25, 8}, {25, 24}, {26, 1}, {27, 1}
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        memcpy(changed, payload, length);
        changed[invalid[i].offset] = invalid[i].value;
        assert(!parsePcm(changed, length));
    }
    length = makePcm(payload, 481, 0, 0);
    assert(!parsePcm(payload, length));
    assert(pcmCalls == before);
}

static void testIrBoundsAndValues(void) {
    uint8_t payload[73], changed[73];
    makeIr(payload);
    assert(parseIr(payload, 72));
    assert(copiedIr.controllerNumber == 15 && copiedIr.flags == 15);
    assert(copiedIr.sourceSequenceNumber == UINT32_C(0x89abcdef));
    assert(copiedIr.timestampUs == UINT64_C(0xfedcba9876543210));
    assert(copiedIr.sourceFrameCount == 480 && copiedIr.laneCorrelation == -0.5f);
    for (int lane = 0; lane < 2; ++lane) {
        assert(copiedIr.lanes[lane].rmsAmplitude == 0.25f);
        assert(copiedIr.lanes[lane].peakAmplitude == 0.75f);
        assert(copiedIr.lanes[lane].transientStrength == 0.5f);
        assert(copiedIr.lanes[lane].lowBandRatio == 1.0f);
        assert(copiedIr.lanes[lane].zeroCrossingRateHz == 48000.0f);
    }
    memset(payload, 0, sizeof(payload));
    assert(copiedIr.laneCorrelation == -0.5f); // Retained frame owns all of its scalar data.
    makeIr(payload);
    put16(payload + 4, 0);
    put32(payload + 8, UINT32_MAX);
    put32(payload + 20, 0);
    payload[1] = LI_DS5_HAPTICS_IR_FLAG_STREAM_END;
    assert(parseIr(payload, 72));
    assert(copiedIr.sourceFrameCount == 0 && copiedIr.sourceSequenceNumber == UINT32_MAX);
    assert(copiedIr.flags == LI_DS5_HAPTICS_IR_FLAG_STREAM_END);

    int before = irCalls;
    assert(!parseIr(NULL, 72));
    assert(!processDs5HapticsIrStreamPacket(payload, 72, NULL, &callbackContext));
    const int badLengths[] = {-1, 0, 71, 73, UINT16_MAX + 1};
    for (size_t i = 0; i < sizeof(badLengths) / sizeof(badLengths[0]); ++i)
        assert(!parseIr(payload, badLengths[i]));
    const struct { int offset; uint8_t value; } invalid[] = {
        {0, 1}, {0, 3}, {1, 16}, {1, 0x80}, {2, 71}, {2, 73},
        {3, 1}, {4, 16}, {5, 0xff}, {6, 1}, {7, 1}, {68, 1}, {71, 1}
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        memcpy(changed, payload, 72);
        changed[invalid[i].offset] = invalid[i].value;
        assert(!parseIr(changed, 72));
    }
    const float invalidNormalized[] = {-0.01f, 1.01f, NAN, INFINITY};
    for (int lane = 0; lane < 2; ++lane) {
        for (int field = 0; field < 4; ++field) {
            for (size_t i = 0; i < sizeof(invalidNormalized) / sizeof(invalidNormalized[0]); ++i) {
                memcpy(changed, payload, 72);
                putFloat(changed + 24 + lane * 20 + field * 4, invalidNormalized[i]);
                assert(!parseIr(changed, 72));
            }
        }
        const float invalidRates[] = {-1.0f, 48001.0f, NAN, INFINITY};
        for (size_t i = 0; i < sizeof(invalidRates) / sizeof(invalidRates[0]); ++i) {
            memcpy(changed, payload, 72);
            putFloat(changed + 40 + lane * 20, invalidRates[i]);
            assert(!parseIr(changed, 72));
        }
        memcpy(changed, payload, 72);
        putFloat(changed + 24 + lane * 20, 0.8f); // RMS must not exceed peak.
        assert(!parseIr(changed, 72));
    }
    const float invalidCorrelations[] = {-1.01f, 1.01f, NAN, INFINITY};
    for (size_t i = 0; i < sizeof(invalidCorrelations) / sizeof(invalidCorrelations[0]); ++i) {
        memcpy(changed, payload, 72);
        putFloat(changed + 64, invalidCorrelations[i]);
        assert(!parseIr(changed, 72));
    }
    assert(irCalls == before);
}

static void testNegotiationProfiles(void) {
    assert(ML_FF_HOST_SBS_TELEMETRY_V2 == 0x04 && ML_FF_ATOMIC_PRESENTATION_MODE_V2 == 0x08);
    assert(ML_FF_SOURCE_FRAME_ID_V1 == 0x10);
    assert(ML_FF_DS5_HAPTICS_PCM == 0x20 && ML_FF_DS5_HAPTICS_IR_V2 == 0x40);
    assert(LI_FF_DS5_HAPTICS_PCM == 0x80);
    assert(LI_FF_DS5_HAPTICS_IR_V2 == 0x04000000 && LI_FF_DS5_HAPTICS_CAPABILITIES_V2 == 0x08000000);
    const uint32_t marker = LI_FF_DS5_HAPTICS_CAPABILITIES_V2;
    assert(!supportsDs5HapticsPcm(0) && !supportsDs5HapticsIrV2(0));
    assert(!supportsDs5HapticsPcm(marker) && !supportsDs5HapticsIrV2(marker));
    assert(usesLegacyDs5HapticsCapabilities(LI_FF_DS5_HAPTICS_PCM));
    assert(getDs5HapticsClientFeatureFlags(LI_FF_DS5_HAPTICS_PCM, false, false) == 0);
    assert(getDs5HapticsClientFeatureFlags(LI_FF_DS5_HAPTICS_PCM, true, false) == 0x04);
    assert(getDs5HapticsClientFeatureFlags(LI_FF_DS5_HAPTICS_PCM, false, true) == 0x08);
    uint32_t versionedPcm = marker | LI_FF_DS5_HAPTICS_PCM;
    assert(!usesLegacyDs5HapticsCapabilities(versionedPcm));
    assert(supportsDs5HapticsPcm(versionedPcm) && !supportsDs5HapticsIrV2(versionedPcm));
    assert(getDs5HapticsClientFeatureFlags(versionedPcm, false, true) == 0);
    assert(getDs5HapticsClientFeatureFlags(versionedPcm, true, false) == 0x20);
    uint32_t versionedBoth = versionedPcm | LI_FF_DS5_HAPTICS_IR_V2;
    assert(supportsDs5HapticsIrV2(versionedBoth));
    assert(getDs5HapticsClientFeatureFlags(versionedBoth, true, false) == 0x20);
    assert(getDs5HapticsClientFeatureFlags(versionedBoth, false, true) == 0x40);
    assert(getDs5HapticsClientFeatureFlags(versionedBoth, false, false) == 0);
    assert(getDs5HapticsClientFeatureFlags(versionedBoth, true, true) == 0);
    assert(getDs5HapticsClientFeatureFlags(LI_FF_DS5_HAPTICS_PCM, true, true) == 0);
    const uint32_t shared[] = {LI_FF_HOST_SBS_TELEMETRY_V2, LI_FF_ATOMIC_PRESENTATION_MODE_V2, LI_FF_SOURCE_FRAME_ID_V1};
    for (unsigned int mask = 1; mask < 8; ++mask) {
        uint32_t host = 0;
        for (int i = 0; i < 3; ++i) if (mask & (1U << i)) host |= shared[i];
        // A reused legacy bit in a shared SBS profile cannot turn haptics on.
        for (int legacyBit = 0; legacyBit < 2; ++legacyBit) {
            uint32_t flags = host | (legacyBit ? LI_FF_DS5_HAPTICS_PCM : 0);
            assert(!usesLegacyDs5HapticsCapabilities(flags));
            assert(!supportsDs5HapticsPcm(flags) && !supportsDs5HapticsIrV2(flags));
            assert(getDs5HapticsClientFeatureFlags(flags, true, false) == 0);
            assert(getDs5HapticsClientFeatureFlags(flags, false, true) == 0);
        }
        host |= versionedBoth;
        assert(getDs5HapticsClientFeatureFlags(host, true, false) == 0x20);
        assert(getDs5HapticsClientFeatureFlags(host, false, true) == 0x40);
    }
}

static bool dispatch(uint16_t type, const uint8_t* payload, int length) {
    int total = sizeof(NVCTL_ENET_PACKET_HEADER_V1) + length;
    PNVCTL_ENET_PACKET_HEADER_V1 packet = malloc(total);
    assert(packet);
    packet->type = type;
    memcpy(packet + 1, payload, length);
    if (type == DS5_HAPTICS_PCM_CONTROL_TYPE) expectedBorrowedPcm = (const uint8_t*)(packet + 1) + 28;
    bool handled = dispatchAuthoredHaptics(packet, total);
    memset(packet, 0xa5, total);
    free(packet);
    return handled;
}

static void testControlRouting(void) {
    uint8_t pcm[32], ir[72];
    makePcm(pcm, 1, 0, 1);
    makeIr(ir);
    packetTypes = (short*)packetTypesGen7Enc;
    int pcmBefore = pcmCalls, irBefore = irCalls;
    SunshineFeatureFlags = LI_FF_DS5_HAPTICS_CAPABILITIES_V2 | LI_FF_DS5_HAPTICS_PCM | LI_FF_DS5_HAPTICS_IR_V2;
    memset(&ListenerCallbacks, 0, sizeof(ListenerCallbacks));
    assert(dispatch(0x550a, pcm, sizeof(pcm)) && dispatch(0x550b, ir, sizeof(ir)));
    assert(pcmCalls == pcmBefore && irCalls == irBefore); // No Android renderer registered.
    ListenerCallbacks.ds5HapticsPcm = receivePcm;
    assert(dispatch(0x550a, pcm, sizeof(pcm)));
    assert(pcmCalls == ++pcmBefore && copiedPcm.pcmData[1] == 0x80);
    assert(dispatch(0x550b, ir, sizeof(ir)) && irCalls == irBefore); // IR is never implicit.
    ListenerCallbacks.ds5HapticsPcm = NULL;
    ListenerCallbacks.ds5HapticsIrV2 = receiveIr;
    assert(dispatch(0x550b, ir, sizeof(ir)) && irCalls == ++irBefore);
    assert(dispatch(0x550a, pcm, sizeof(pcm)) && pcmCalls == pcmBefore);
    SunshineFeatureFlags &= ~LI_FF_DS5_HAPTICS_IR_V2;
    assert(dispatch(0x550b, ir, sizeof(ir)) && irCalls == irBefore);
    SunshineFeatureFlags = LI_FF_DS5_HAPTICS_PCM; // Explicit IR opt-in on legacy hosts remains supported.
    assert(dispatch(0x550b, ir, sizeof(ir)) && irCalls == ++irBefore);
    ListenerCallbacks.ds5HapticsIrV2 = NULL;
    ListenerCallbacks.ds5HapticsPcm = receivePcm;
    assert(dispatch(0x550a, pcm, sizeof(pcm)) && pcmCalls == ++pcmBefore);
    SunshineFeatureFlags |= LI_FF_HOST_SBS_TELEMETRY_V2;
    assert(dispatch(0x550a, pcm, sizeof(pcm)) && pcmCalls == pcmBefore);
    SunshineFeatureFlags = 0;
    assert(dispatch(0x550a, pcm, sizeof(pcm)) && pcmCalls == pcmBefore);
    SunshineFeatureFlags = LI_FF_DS5_HAPTICS_CAPABILITIES_V2 | LI_FF_DS5_HAPTICS_PCM;
    pcm[0] = 2;
    assert(dispatch(0x550a, pcm, sizeof(pcm)) && pcmCalls == pcmBefore);
    pcm[0] = 1;
    assert(dispatch(0x550a, pcm, sizeof(pcm) - 1) && pcmCalls == pcmBefore);
    packetTypes = (short*)packetTypesGen7;
    assert(!dispatch(0x550a, pcm, sizeof(pcm)) && pcmCalls == pcmBefore);
    packetTypes = (short*)packetTypesGen7Enc;
    AppVersionQuad[3] = 0;
    assert(!dispatch(0x550a, pcm, sizeof(pcm)) && pcmCalls == pcmBefore);
    AppVersionQuad[3] = -1;
    assert(!dispatch(0x7777, pcm, sizeof(pcm)));
    NVCTL_ENET_PACKET_HEADER_V1 truncated = {0};
    assert(!dispatchAuthoredHaptics(&truncated, sizeof(truncated) - 1));
}

static void rumble(unsigned short controller, unsigned short low, unsigned short high) {
    assert(controller == 3 && low == 0x1234 && high == 0xabcd); ++standardCalls;
}
static void triggerRumble(uint16_t controller, uint16_t left, uint16_t right) {
    assert(controller == 3 && left == 0x1234 && right == 0xabcd); ++standardCalls;
}
static void motion(uint16_t controller, uint8_t type, uint16_t rate) {
    assert(controller == 3 && type == 2 && rate == 250); ++standardCalls;
}
static void led(uint16_t controller, uint8_t r, uint8_t g, uint8_t b) {
    assert(controller == 3 && r == 0x12 && g == 0x34 && b == 0x56); ++standardCalls;
}
static void adaptive(uint16_t controller, uint8_t flags, uint8_t leftType, uint8_t rightType, uint8_t* left, uint8_t* right) {
    assert(controller == 3 && flags == 0x0c && leftType == 1 && rightType == 2);
    for (int i = 0; i < DS_EFFECT_PAYLOAD_SIZE; ++i) assert(left[i] == i && right[i] == 0x80 + i);
    ++standardCalls;
}

static void queueStandard(int index, const uint8_t* payload, int length) {
    int total = sizeof(NVCTL_ENET_PACKET_HEADER_V1) + length;
    PNVCTL_ENET_PACKET_HEADER_V1 packet = malloc(total);
    assert(packet);
    packet->type = packetTypes[index];
    memcpy(packet + 1, payload, length);
    assert(!dispatchAuthoredHaptics(packet, total));
    assert(needsAsyncCallback(packet->type));
    queueAsyncCallback(packet, total);
    memset(packet, 0, total); // The existing callback queue must still copy its payloads.
    free(packet);
}

static void testStandardControllerCallbacks(void) {
    const uint8_t rumbleBody[] = {0, 0, 0, 0, 3, 0, 0x34, 0x12, 0xcd, 0xab};
    const uint8_t triggerBody[] = {3, 0, 0x34, 0x12, 0xcd, 0xab};
    const uint8_t motionBody[] = {3, 0, 250, 0, 2};
    const uint8_t ledBody[] = {3, 0, 0x12, 0x34, 0x56};
    uint8_t adaptiveBody[5 + 2 * DS_EFFECT_PAYLOAD_SIZE] = {3, 0, 0x0c, 1, 2};
    for (int i = 0; i < DS_EFFECT_PAYLOAD_SIZE; ++i) { adaptiveBody[5 + i] = i; adaptiveBody[15 + i] = 0x80 + i; }
    ListenerCallbacks.rumble = rumble;
    ListenerCallbacks.rumbleTriggers = triggerRumble;
    ListenerCallbacks.setMotionEventState = motion;
    ListenerCallbacks.setControllerLED = led;
    ListenerCallbacks.setAdaptiveTriggers = adaptive;
    int pcmBefore = pcmCalls, irBefore = irCalls;
    assert(LbqInitializeLinkedBlockingQueue(&asyncCallbackQueue, 16) == 0);
    queueStandard(IDX_RUMBLE_DATA, rumbleBody, sizeof(rumbleBody));
    queueStandard(IDX_RUMBLE_TRIGGER_DATA, triggerBody, sizeof(triggerBody));
    queueStandard(IDX_SET_MOTION_EVENT, motionBody, sizeof(motionBody));
    queueStandard(IDX_SET_RGB_LED, ledBody, sizeof(ledBody));
    queueStandard(IDX_DS_ADAPTIVE_TRIGGERS, adaptiveBody, sizeof(adaptiveBody));
    assert(LbqGetItemCount(&asyncCallbackQueue) == 5);
    LbqSignalQueueDrain(&asyncCallbackQueue);
    asyncCallbackThreadFunc(NULL);
    assert(standardCalls == 5 && pcmCalls == pcmBefore && irCalls == irBefore);
    assert(LbqDestroyLinkedBlockingQueue(&asyncCallbackQueue) == NULL);
}

int main(void) {
    testPcmBoundsAndOwnership();
    testIrBoundsAndValues();
    testNegotiationProfiles();
    testControlRouting();
    testStandardControllerCallbacks();
    return 0;
}
