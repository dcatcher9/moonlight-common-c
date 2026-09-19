#include "Ds5HapticsStream.h"
#include "Limelight-internal.h"

#include <string.h>

bool usesLegacyDs5HapticsCapabilities(uint32_t hostFeatures) {
    const uint32_t sharedProfile = LI_FF_GAME_PROVIDER_V1 | LI_FF_HOST_SBS_TELEMETRY_V2 |
                                   LI_FF_ATOMIC_PRESENTATION_MODE_V2 |
                                   LI_FF_SOURCE_FRAME_ID_V1 |
                                   LI_FF_DS5_HAPTICS_IR_V2 |
                                   LI_FF_DS5_HAPTICS_CAPABILITIES_V2;
    return (hostFeatures & LI_FF_DS5_HAPTICS_PCM) && !(hostFeatures & sharedProfile);
}

bool supportsDs5HapticsPcm(uint32_t hostFeatures) {
    return (hostFeatures & LI_FF_DS5_HAPTICS_PCM) &&
           (usesLegacyDs5HapticsCapabilities(hostFeatures) ||
            (hostFeatures & LI_FF_DS5_HAPTICS_CAPABILITIES_V2));
}

bool supportsDs5HapticsIrV2(uint32_t hostFeatures) {
    return usesLegacyDs5HapticsCapabilities(hostFeatures) ||
           ((hostFeatures & LI_FF_DS5_HAPTICS_CAPABILITIES_V2) &&
            (hostFeatures & LI_FF_DS5_HAPTICS_IR_V2));
}

uint32_t getDs5HapticsClientFeatureFlags(uint32_t hostFeatures, bool pcm, bool ir) {
    if (pcm && ir) {
        return 0; // The connection entry point rejects ambiguous registrations.
    }
    if (usesLegacyDs5HapticsCapabilities(hostFeatures)) {
        // These values belong exclusively to the legacy host profile; they must
        // never be interpreted as authored haptics by a shared-profile host.
        return pcm ? 0x04 : ir ? 0x08 : 0;
    }
    if (pcm && supportsDs5HapticsPcm(hostFeatures)) {
        return ML_FF_DS5_HAPTICS_PCM;
    }
    if (ir && supportsDs5HapticsIrV2(hostFeatures)) {
        return ML_FF_DS5_HAPTICS_IR_V2;
    }
    return 0;
}

static uint16_t readLe16(const uint8_t* data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t readLe32(const uint8_t* data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static uint64_t readLe64(const uint8_t* data) {
    return (uint64_t)readLe32(data) | ((uint64_t)readLe32(data + 4) << 32);
}

bool processDs5HapticsStreamPacket(const uint8_t* payload,
                                   int payloadLength,
                                   Ds5HapticsStreamCallback callback,
                                   void* context) {
    LI_DS5_HAPTICS_PCM_FRAME frame;
    uint16_t headerSize;
    uint16_t reserved;
    uint32_t expectedPcmBytes;
    const uint8_t knownFlags = LI_DS5_HAPTICS_PCM_FLAG_STREAM_START |
                               LI_DS5_HAPTICS_PCM_FLAG_STREAM_END |
                               LI_DS5_HAPTICS_PCM_FLAG_DISCONTINUITY;

    if (payload == NULL || callback == NULL ||
            payloadLength < DS5_HAPTICS_STREAM_WIRE_HEADER_SIZE ||
            payloadLength > UINT16_MAX) {
        return false;
    }

    memset(&frame, 0, sizeof(frame));
    frame.flags = payload[1];
    headerSize = readLe16(payload + 2);
    frame.controllerNumber = readLe16(payload + 4);
    frame.frameCount = readLe16(payload + 6);
    frame.sequenceNumber = readLe32(payload + 8);
    frame.presentationTimeUs = readLe64(payload + 12);
    frame.sampleRate = readLe32(payload + 20);
    frame.channelCount = payload[24];
    frame.bitsPerSample = payload[25];
    reserved = readLe16(payload + 26);

    if (payload[0] != DS5_HAPTICS_STREAM_PROTOCOL_VERSION ||
            (frame.flags & ~knownFlags) != 0 || reserved != 0 ||
            headerSize < DS5_HAPTICS_STREAM_WIRE_HEADER_SIZE ||
            headerSize > (uint16_t)payloadLength ||
            frame.sampleRate != 48000 || frame.channelCount != 2 ||
            frame.bitsPerSample != 16 ||
            frame.controllerNumber >= 16 ||
            frame.frameCount > DS5_HAPTICS_STREAM_MAX_FRAMES) {
        return false;
    }

    expectedPcmBytes = (uint32_t)frame.frameCount * frame.channelCount *
                       (frame.bitsPerSample / 8);
    if ((uint32_t)payloadLength != (uint32_t)headerSize + expectedPcmBytes) {
        return false;
    }

    frame.pcmData = payload + headerSize;
    frame.pcmDataLength = expectedPcmBytes;
    callback(&frame, context);
    return true;
}
