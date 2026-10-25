// raknet.hpp - RakNet "unconnected ping/pong", the packets Bedrock uses for the server list.
#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace bk {

constexpr uint8_t kPacketPing = 0x01;       // unconnected ping
constexpr uint8_t kPacketPingOpen = 0x02;   // unconnected ping (open connections only)
constexpr uint8_t kPacketPong = 0x1C;       // unconnected pong
constexpr uint8_t kMagic[16] = {0x00, 0xFF, 0xFF, 0x00, 0xFE, 0xFE, 0xFE, 0xFE,
                                0xFD, 0xFD, 0xFD, 0xFD, 0x12, 0x34, 0x56, 0x78};

inline void put_be(std::vector<uint8_t>& v, uint64_t x, int bytes) {
    for (int k = bytes - 1; k >= 0; --k) v.push_back((uint8_t)(x >> (8 * k)));
}
inline uint64_t get_be(const uint8_t* p, int bytes) {
    uint64_t x = 0;
    for (int k = 0; k < bytes; ++k) x = (x << 8) | p[k];
    return x;
}

inline std::vector<uint8_t> build_ping(uint64_t time_ms, uint64_t client_guid) {
    std::vector<uint8_t> p;
    p.push_back(kPacketPing);
    put_be(p, time_ms, 8);
    p.insert(p.end(), kMagic, kMagic + 16);
    put_be(p, client_guid, 8);
    return p;
}

inline bool is_ping(const std::vector<uint8_t>& p) {
    return p.size() >= 25 && (p[0] == kPacketPing || p[0] == kPacketPingOpen) && std::memcmp(&p[9], kMagic, 16) == 0;
}

inline std::vector<uint8_t> build_pong(const std::vector<uint8_t>& ping, uint64_t server_guid, const std::string& ad) {
    std::vector<uint8_t> p;
    p.push_back(kPacketPong);
    p.insert(p.end(), ping.begin() + 1, ping.begin() + 9);  // echo client timestamp
    put_be(p, server_guid, 8);
    p.insert(p.end(), kMagic, kMagic + 16);
    put_be(p, ad.size(), 2);
    p.insert(p.end(), ad.begin(), ad.end());
    return p;
}

struct ServerInfo {
    std::string edition, motd, version, level_name, gamemode;
    int protocol = 0, players = 0, max_players = 0;
    uint64_t server_guid = 0;
    std::string port4, port6, raw;
};

inline std::vector<std::string> split_semicolons(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    for (;;) {
        size_t n = s.find(';', pos);
        out.push_back(s.substr(pos, n == std::string::npos ? std::string::npos : n - pos));
        if (n == std::string::npos) break;
        pos = n + 1;
    }
    return out;
}

inline int to_int(const std::string& s) { return s.empty() ? 0 : std::atoi(s.c_str()); }

inline ServerInfo parse_pong(const std::vector<uint8_t>& p) {
    if (p.size() < 35 || p[0] != kPacketPong) throw std::runtime_error("not an unconnected pong");
    if (std::memcmp(&p[17], kMagic, 16) != 0) throw std::runtime_error("bad RakNet magic");
    size_t len = (size_t)get_be(&p[33], 2);
    if (35 + len > p.size()) throw std::runtime_error("truncated pong");
    ServerInfo info;
    info.server_guid = get_be(&p[9], 8);
    info.raw.assign((const char*)&p[35], len);
    auto f = split_semicolons(info.raw);
    auto at = [&](size_t k) { return k < f.size() ? f[k] : std::string(); };
    info.edition = at(0);
    info.motd = at(1);
    info.protocol = to_int(at(2));
    info.version = at(3);
    info.players = to_int(at(4));
    info.max_players = to_int(at(5));
    info.level_name = at(7);
    info.gamemode = at(8);
    info.port4 = at(10);
    info.port6 = at(11);
    return info;
}

// Removes Minecraft formatting codes (the section sign followed by one character).
inline std::string strip_formatting(const std::string& s) {
    std::string out;
    for (size_t k = 0; k < s.size(); ++k) {
        if ((uint8_t)s[k] == 0xC2 && k + 1 < s.size() && (uint8_t)s[k + 1] == 0xA7) { k += 2; continue; }
        out += s[k];
    }
    return out;
}

}  // namespace bk
