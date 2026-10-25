// net.hpp - tiny cross-platform (Windows/Linux/macOS) UDP socket wrapper.
#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #ifdef _MSC_VER
  #pragma comment(lib, "ws2_32.lib")
  #endif
  using socket_t = SOCKET;
  static const socket_t kBadSocket = INVALID_SOCKET;
  inline int close_socket(socket_t s) { return closesocket(s); }
#else
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using socket_t = int;
  static const socket_t kBadSocket = -1;
  inline int close_socket(socket_t s) { return close(s); }
#endif

namespace bk {

struct NetInit {
    NetInit() {
#ifdef _WIN32
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d)) throw std::runtime_error("WSAStartup failed");
#endif
    }
    ~NetInit() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

struct Address {
    sockaddr_storage ss{};
    socklen_t len = 0;
    std::string str() const {
        char host[NI_MAXHOST], serv[NI_MAXSERV];
        if (getnameinfo((const sockaddr*)&ss, len, host, sizeof host, serv, sizeof serv, NI_NUMERICHOST | NI_NUMERICSERV))
            return "?";
        return std::string(host) + ":" + serv;
    }
};

inline Address resolve_udp(const std::string& host, uint16_t port) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rc || !res) throw std::runtime_error("cannot resolve host: " + host);
    Address a;
    std::memcpy(&a.ss, res->ai_addr, res->ai_addrlen);
    a.len = (socklen_t)res->ai_addrlen;
    freeaddrinfo(res);
    return a;
}

class UdpSocket {
public:
    UdpSocket() {
        s_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_ == kBadSocket) throw std::runtime_error("cannot create UDP socket");
    }
    ~UdpSocket() { if (s_ != kBadSocket) close_socket(s_); }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    void bind_any(uint16_t port) {
        int yes = 1;
        setsockopt(s_, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(port);
        if (::bind(s_, (sockaddr*)&a, sizeof a) != 0)
            throw std::runtime_error("cannot bind UDP port " + std::to_string(port) + " (already in use?)");
    }
    void send_to(const Address& to, const std::vector<uint8_t>& data) {
        ::sendto(s_, (const char*)data.data(), (int)data.size(), 0, (const sockaddr*)&to.ss, to.len);
    }
    // Returns false on timeout.
    bool recv_from(std::vector<uint8_t>& buf, Address& from, int timeout_ms) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s_, &fds);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (select((int)s_ + 1, &fds, nullptr, nullptr, &tv) <= 0) return false;
        buf.resize(4096);
        from.len = sizeof from.ss;
        int n = ::recvfrom(s_, (char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&from.ss, &from.len);
        if (n <= 0) return false;
        buf.resize((size_t)n);
        return true;
    }
private:
    socket_t s_;
};

}  // namespace bk
