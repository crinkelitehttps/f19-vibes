#include "host/headtrack.h"

#include <SDL3/SDL.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>

namespace f19 {

HeadTracker::~HeadTracker() {
    if (fd_ >= 0) close(fd_);
}

bool HeadTracker::open(const char* addr, int port) {
    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd_ < 0) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(uint16_t(port));
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1 || bind(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) < 0) {
        close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

HeadPose HeadTracker::poll() {
    if (fd_ < 0) return {};
    double d[6];
    ssize_t n;
    while ((n = recv(fd_, d, sizeof d, 0)) >= 0) {
        if (n != sizeof d) continue;
        bool ok = true;
        for (double v : d) ok = ok && std::isfinite(v);
        if (!ok) continue;
        const float deg = 3.14159265f / 180.0f;
        pose_ = HeadPose{float(d[3]) * deg, float(d[4]) * deg, float(d[5]) * deg, float(d[0]), float(d[1]), float(d[2])};
        last_ns_ = SDL_GetTicksNS();
        if (!announced_) {
            announced_ = true;
            std::fprintf(stderr, "head tracking: receiving\n");
        }
    }
    // Recentre when the tracker stops sending.
    if (last_ns_ && SDL_GetTicksNS() - last_ns_ > 1000000000ull) {
        pose_ = {};
        last_ns_ = 0;
        announced_ = false;
        std::fprintf(stderr, "head tracking: no data\n");
    }
    return pose_;
}

}  // namespace f19
