#include "host/headtrack.h"

#include <SDL3/SDL.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cmath>
#include <cstdio>

namespace f19 {

namespace {

#ifdef _WIN32
using Socket = SOCKET;
void close_socket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket INVALID_SOCKET = -1;
void close_socket(Socket s) { close(s); }
#endif

}  // namespace

HeadTracker::~HeadTracker() {
    if (fd_ >= 0) close_socket(Socket(fd_));
}

bool HeadTracker::open(const char* addr, int port) {
#ifdef _WIN32
    static bool wsa = [] {
        WSADATA wd;
        return WSAStartup(MAKEWORD(2, 2), &wd) == 0;
    }();
    if (!wsa) return false;
#endif
    Socket s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return false;
#ifdef _WIN32
    u_long nonblocking = 1;
    bool ok = ioctlsocket(s, FIONBIO, &nonblocking) == 0;
#else
    bool ok = fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK) == 0;
#endif
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(uint16_t(port));
    if (!ok || inet_pton(AF_INET, addr, &sa.sin_addr) != 1 || bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof sa) < 0) {
        close_socket(s);
        return false;
    }
    fd_ = intptr_t(s);
    return true;
}

HeadPose HeadTracker::poll() {
    if (fd_ < 0) return {};
    double d[6];
    int n;
    while ((n = int(recv(Socket(fd_), reinterpret_cast<char*>(d), sizeof d, 0))) >= 0) {
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
