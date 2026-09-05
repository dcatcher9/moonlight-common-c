#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// This maximum has the same units as x-nv-video[0].packetSize: NV header + video
// payload, excluding the 16-byte RTP and 32-byte encrypted-video headers.
// Zero means the optional extension is absent. On malformed/duplicate input, leave
// the caller's maximum alone and fail the handshake rather than ignoring a limit.
static inline bool parseVideoPacketSizeMaximum(const char* sdp, int* selected) {
    static const char attribute[] = "a=x-ss-video[0].maxPacketSize:";
    bool found = false;
    int result = 0;
    const char* line = sdp;
    while (*line != '\0') {
        const char* end = strchr(line, '\n');
        const char* next = end ? end + 1 : line + strlen(line);
        if (!end) end = next;
        if (end > line && end[-1] == '\r') --end;
        if ((size_t)(end - line) >= sizeof(attribute) - 1 &&
                memcmp(line, attribute, sizeof(attribute) - 1) == 0) {
            unsigned int maximum = 0;
            const char* value = line + sizeof(attribute) - 1;
            if (found || value == end) return false;
            found = true;
            for (; value != end; ++value) {
                if (*value < '0' || *value > '9' || maximum > 65459U / 10U) return false;
                maximum = maximum * 10U + (unsigned int)(*value - '0');
                if (maximum > 65459U) return false;
            }
            if (maximum < 200U) return false;
            result = (int)maximum;
        }
        line = next;
    }
    *selected = result;
    return true;
}
