// Head tracking input: OpenTrack's "UDP over network" output.
//
// Each datagram is six little-endian doubles: x, y, z (cm), yaw, pitch,
// roll (degrees). Signs as delivered by OpenTrack's defaults: yaw right +,
// pitch up +, roll right ear down +, x right +, y up +, z back + (flip axes
// in OpenTrack's Options > Output if one feels reversed).
#pragma once

#include <cstdint>

#include "hires/gl_render.h"

namespace f19 {

class HeadTracker {
public:
    ~HeadTracker();
    // Listen on addr:port (UDP). Returns false if the socket can't be bound.
    bool open(const char* addr, int port);
    // Drain pending datagrams; returns the latest pose, or the zero pose if
    // nothing has arrived for a while (tracker stopped).
    HeadPose poll();

private:
    int fd_ = -1;
    HeadPose pose_{};
    uint64_t last_ns_ = 0;
    bool announced_ = false;
};

}  // namespace f19
