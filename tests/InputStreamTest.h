#pragma once

#include <stdbool.h>
#include <stdint.h>

enum LiTestInputPacketType {
    LI_TEST_INPUT_PACKET_RELATIVE_MOVE,
    LI_TEST_INPUT_PACKET_ABSOLUTE_MOVE,
    LI_TEST_INPUT_PACKET_MOUSE_BUTTON,
    LI_TEST_INPUT_PACKET_KEYBOARD,
    LI_TEST_INPUT_PACKET_SCROLL,
    LI_TEST_INPUT_PACKET_OTHER,
};

typedef struct LiTestInputPacket {
    enum LiTestInputPacketType type;
    int64_t x;
    int64_t y;
    int width;
    int height;
    int button;
    bool moreData;
} LiTestInputPacket;

int LiTestInitializeInputStream(void);
void LiTestDestroyInputStream(void);
int LiTestPollInputPacket(LiTestInputPacket* packet);
bool LiTestHasPendingRelativeMouseHolder(void);
bool LiTestHasPendingAbsoluteMouseHolder(void);
int LiTestGetEmptyRelativeMotionFlushCount(void);
