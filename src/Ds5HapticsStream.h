#pragma once

#include "Limelight.h"

#define DS5_HAPTICS_STREAM_PROTOCOL_VERSION 1
#define DS5_HAPTICS_STREAM_WIRE_HEADER_SIZE 28
#define DS5_HAPTICS_STREAM_MAX_FRAMES 480
#define DS5_HAPTICS_PCM_CONTROL_TYPE 0x550A
#define DS5_HAPTICS_IR_CONTROL_TYPE 0x550B

// Legacy Foundation hosts used the client SBS bits for authored haptics. Select
// that profile only when no shared SBS or versioned haptics capability is present.
bool usesLegacyDs5HapticsCapabilities(uint32_t hostFeatures);
bool supportsDs5HapticsPcm(uint32_t hostFeatures);
bool supportsDs5HapticsIrV2(uint32_t hostFeatures);
uint32_t getDs5HapticsClientFeatureFlags(uint32_t hostFeatures, bool pcm, bool ir);

typedef void(*Ds5HapticsStreamCallback)(const LI_DS5_HAPTICS_PCM_FRAME* frame,
                                        void* context);

bool processDs5HapticsStreamPacket(const uint8_t* payload,
                                   int payloadLength,
                                   Ds5HapticsStreamCallback callback,
                                   void* context);
