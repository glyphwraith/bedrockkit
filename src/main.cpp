// bedrockkit - tools for Minecraft: Bedrock Edition.
//   bedrockkit ping  <host> [port]      query a server's MOTD / players / version
//   bedrockkit serve                    run a mock server that answers pings (for testing)
//   bedrockkit level <level.dat>        inspect (and edit) a world's level.dat
//   bedrockkit nbt   <file>             dump any NBT file
//   bedrockkit sample <out.dat>         write a sample level.dat to experiment with
#include <atomic>
#include <chrono>
#include <csignal>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include "bedrockkit/net.hpp"
#include "bedrockkit/nbt.hpp"
#include "bedrockkit/raknet.hpp"

using namespace bk;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------- argument parsing
struct Args {
    std::vector<std::string> pos;
    std::map<std::string, std::vector<std::string>> opts;
    std::set<std::string> flags;

    Args(int argc, char** argv, const std::set<std::string>& value_opts) {
        for (int k = 2; k < argc; ++k) {
            std::string a = argv[k];
            if (a.rfind("--", 0) != 0) { pos.push_back(a); continue; }
            if (value_opts.count(a)) {
                if (k + 1 >= argc) throw std::runtime_error("missing value for " + a);
                opts[a].push_back(argv[++k]);
            } else flags.insert(a);
        }
    }
    bool flag(const std::string& f) const { return flags.count(f) > 0; }
    std::string get(const std::string& o, const std::string& def) const {
        auto it = opts.find(o);
        return it == opts.end() ? def : it->second.back();
    }
    int get_int(const std::string& o, int def) const {
        auto it = opts.find(o);
        return it == opts.end() ? def : std::atoi(it->second.back().c_str());
    }
};

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
static void write_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    f.write((const char*)data.data(), (std::streamsize)data.size());
}

// ---------------------------------------------------------------- level.dat
// Layout: int32 LE storage version, int32 LE payload length, then little-endian NBT.
static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

struct LevelFile {
    int32_t storage_version = 10;
    std::string root_name;
    Tag root;
};

static LevelFile parse_level(const std::vector<uint8_t>& data) {
    if (data.size() < 9) throw std::runtime_error("file too small to be a level.dat");
    LevelFile lf;
    lf.storage_version = (int32_t)le32(data.data());
    uint32_t len = le32(data.data() + 4);
    size_t avail = data.size() - 8;
    if (len > avail) throw std::runtime_error("level.dat header says " + std::to_string(len) +
                                              " bytes of NBT but only " + std::to_string(avail) + " are present");
    Reader r(data.data() + 8, len, true);
    auto [name, tag] = r.read_root();
    if (tag.type != TagType::Compound) throw std::runtime_error("level.dat root is not a compound");
    lf.root_name = name;
    lf.root = std::move(tag);
    return lf;
}

static std::vector<uint8_t> serialize_level(const LevelFile& lf) {
    std::vector<uint8_t> nbt = Writer(true).write_root(lf.root_name, lf.root);
    std::vector<uint8_t> out;
    for (int k = 0; k < 4; ++k) out.push_back((uint8_t)((uint32_t)lf.storage_version >> (8 * k)));
    for (int k = 0; k < 4; ++k) out.push_back((uint8_t)((uint32_t)nbt.size() >> (8 * k)));
    out.insert(out.end(), nbt.begin(), nbt.end());
    return out;
}

static std::string name_of(int64_t v, const std::map<int64_t, const char*>& names) {
    auto it = names.find(v);
    return it == names.end() ? std::to_string(v) : std::string(it->second) + " (" + std::to_string(v) + ")";
}

static void print_level_summary(const LevelFile& lf) {
    const Tag& r = lf.root;
    auto num = [&](const char* k, int64_t& out) {
        const Tag* t = r.find(k);
        if (!t || !(is_scalar(t->type)) || t->type == TagType::Float || t->type == TagType::Double) return false;
        out = t->i;
        return true;
    };
    auto row = [](const std::string& label, const std::string& value) {
        std::cout << "  " << label << std::string(label.size() < 22 ? 22 - label.size() : 1, ' ') << value << "\n";
    };
    int64_t v;
    std::cout << "level.dat (storage version " << lf.storage_version << ")\n";
    if (const Tag* t = r.find("LevelName")) row("World name", t->s);
    if (num("RandomSeed", v)) row("Seed", std::to_string(v));
    if (num("GameType", v)) row("Game mode", name_of(v, {{0, "Survival"}, {1, "Creative"}, {2, "Adventure"}, {6, "Spectator"}}));
    if (num("Difficulty", v)) row("Difficulty", name_of(v, {{0, "Peaceful"}, {1, "Easy"}, {2, "Normal"}, {3, "Hard"}}));
    if (num("Generator", v)) row("World type", name_of(v, {{0, "Old"}, {1, "Infinite"}, {2, "Flat"}}));
    if (num("SpawnX", v)) {
        int64_t y = 0, z = 0;
        num("SpawnY", y); num("SpawnZ", z);
        row("Spawn", std::to_string(v) + ", " + std::to_string(y) + ", " + std::to_string(z));
    }
    if (num("Time", v)) row("World time (ticks)", std::to_string(v));
    if (num("commandsEnabled", v)) row("Cheats", v ? "enabled" : "disabled");
    if (num("LastPlayed", v)) {
        std::time_t t = (std::time_t)v;
        char buf[64] = "?";
        if (std::tm* tm = std::gmtime(&t)) std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S UTC", tm);
        row("Last played", buf);
    }
    if (const Tag* t = r.find("lastOpenedWithVersion"); t && t->type == TagType::List) {
        std::string s;
        for (auto& e : t->list) s += (s.empty() ? "" : ".") + std::to_string(e.i);
        row("Last opened with", s);
    }
    if (num("NetworkVersion", v)) row("Protocol version", std::to_string(v));
    std::cout << "  (" << r.keys.size() << " root tags total, use --dump to see all)\n";
}

static int cmd_level(int argc, char** argv) {
    Args a(argc, argv, {"--set", "--get", "--out"});
    if (a.pos.empty()) throw std::runtime_error("usage: bedrockkit level <level.dat> [--dump] [--get Path] [--set Path=Value]... [--out file]");
    const std::string& path = a.pos[0];
    LevelFile lf = parse_level(read_file(path));

    if (a.opts.count("--set")) {
        for (auto& kv : a.opts["--set"]) {
            size_t eq = kv.find('=');
            if (eq == std::string::npos) throw std::runtime_error("--set expects Path=Value, got: " + kv);
            std::string p = kv.substr(0, eq), val = kv.substr(eq + 1);
            Tag* t = resolve(lf.root, p);
            if (!t) throw std::runtime_error("no such tag: " + p);
            assign_from_string(*t, val);
            std::cout << "set " << p << " = " << scalar_str(*t) << "\n";
        }
        std::string out = a.get("--out", path);
        if (out == path) {
            write_file(path + ".bak", read_file(path));
            std::cout << "backup saved to " << path << ".bak\n";
        }
        write_file(out, serialize_level(lf));
        std::cout << "wrote " << out << " (close Minecraft before editing a live world)\n";
        return 0;
    }
    if (a.opts.count("--get")) {
        Tag* t = resolve(lf.root, a.get("--get", ""));
        if (!t) throw std::runtime_error("no such tag: " + a.get("--get", ""));
        snbt(std::cout, *t);
        std::cout << "\n";
        return 0;
    }
    if (a.flag("--dump")) { snbt(std::cout, lf.root); std::cout << "\n"; return 0; }
    print_level_summary(lf);
    return 0;
}

static int cmd_nbt(int argc, char** argv) {
    Args a(argc, argv, {"--skip"});
    if (a.pos.empty()) throw std::runtime_error("usage: bedrockkit nbt <file> [--big-endian] [--skip N]");
    auto data = read_file(a.pos[0]);
    size_t skip = (size_t)a.get_int("--skip", 0);
    if (skip >= data.size()) throw std::runtime_error("--skip is larger than the file");
    Reader r(data.data() + skip, data.size() - skip, !a.flag("--big-endian"));
    auto [name, tag] = r.read_root();
    std::cout << (name.empty() ? "(unnamed root)" : name) << ": ";
    snbt(std::cout, tag);
    std::cout << "\n";
    return 0;
}

static int cmd_sample(int argc, char** argv) {
    Args a(argc, argv, {});
    std::string out = a.pos.empty() ? "level.dat" : a.pos[0];
    LevelFile lf;
    lf.storage_version = 10;
    Tag& r = lf.root = Tag::compound();
    auto I = [](int64_t v) { return Tag::integer(TagType::Int, v); };
    auto B = [](int64_t v) { return Tag::integer(TagType::Byte, v); };
    r.set("LevelName", Tag::string("Sample World"));
    r.set("RandomSeed", Tag::integer(TagType::Long, 1234567890123LL));
    r.set("GameType", I(0));
    r.set("Difficulty", I(2));
    r.set("Generator", I(1));
    r.set("StorageVersion", I(10));
    r.set("NetworkVersion", I(685));
    r.set("SpawnX", I(0)); r.set("SpawnY", I(64)); r.set("SpawnZ", I(0));
    r.set("Time", Tag::integer(TagType::Long, 6000));
    r.set("LastPlayed", Tag::integer(TagType::Long, (int64_t)std::time(nullptr)));
    r.set("commandsEnabled", B(1));
    r.set("dodaylightcycle", B(1));
    r.set("keepinventory", B(0));
    r.set("rainLevel", Tag::floating(TagType::Float, 0.0));
    Tag ver = Tag::make_list(TagType::Int);
    for (int v : {1, 21, 0, 3, 0}) ver.list.push_back(I(v));
    r.set("lastOpenedWithVersion", ver);
    Tag exp = Tag::compound();
    exp.set("experiments_ever_used", B(0));
    exp.set("saved_with_toggled_experiments", B(0));
    r.set("experiments", exp);
    write_file(out, serialize_level(lf));
    std::cout << "wrote " << out << "\ntry:  bedrockkit level " << out << "\n";
    return 0;
}

// ---------------------------------------------------------------- networking
static void print_info(const ServerInfo& s) {
    auto row = [](const char* l, const std::string& v) { std::cout << "  " << l << std::string(14 - std::strlen(l), ' ') << v << "\n"; };
    row("MOTD", strip_formatting(s.motd));
    if (!s.level_name.empty()) row("Level name", strip_formatting(s.level_name));
    row("Edition", s.edition);
    row("Version", s.version + " (protocol " + std::to_string(s.protocol) + ")");
    row("Players", std::to_string(s.players) + " / " + std::to_string(s.max_players));
    if (!s.gamemode.empty()) row("Game mode", s.gamemode);
    if (!s.port4.empty()) row("Ports", "v4 " + s.port4 + (s.port6.empty() ? "" : ", v6 " + s.port6));
}

static int cmd_ping(int argc, char** argv) {
    Args a(argc, argv, {"--count", "--timeout"});
    if (a.pos.empty()) throw std::runtime_error("usage: bedrockkit ping <host> [port] [--count N] [--timeout ms] [--raw]");
    std::string host = a.pos[0];
    int port = a.pos.size() > 1 ? std::atoi(a.pos[1].c_str()) : 19132;
    if (port <= 0 || port > 65535) throw std::runtime_error("invalid port");
    int count = std::max(1, a.get_int("--count", 4)), timeout = std::max(100, a.get_int("--timeout", 2000));

    NetInit net;
    Address target = resolve_udp(host, (uint16_t)port);
    UdpSocket sock;
    std::mt19937_64 rng(std::random_device{}());
    uint64_t guid = rng();
    auto start = Clock::now();

    std::cout << "Pinging " << host << " (" << target.str() << ") ...\n";
    std::vector<double> rtts;
    bool printed = false;
    for (int k = 0; k < count; ++k) {
        uint64_t now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        auto t0 = Clock::now();
        sock.send_to(target, build_ping(now_ms, guid));
        std::vector<uint8_t> buf;
        Address from;
        bool got = false;
        while (std::chrono::duration<double, std::milli>(Clock::now() - t0).count() < timeout) {
            if (!sock.recv_from(buf, from, timeout)) break;
            if (!buf.empty() && buf[0] == kPacketPong) { got = true; break; }
        }
        if (!got) { std::cout << "  request " << (k + 1) << ": timed out\n"; continue; }
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        rtts.push_back(ms);
        std::printf("  reply %d: %.1f ms\n", k + 1, ms);
        if (!printed) {
            printed = true;
            ServerInfo info = parse_pong(buf);
            std::cout << "\n";
            print_info(info);
            if (a.flag("--raw")) std::cout << "  Raw           " << info.raw << "\n";
            std::cout << "\n";
        }
    }
    if (rtts.empty()) { std::cout << "no response. The server may be offline, blocking UDP, or not a Bedrock server.\n"; return 1; }
    double mn = rtts[0], mx = rtts[0], sum = 0;
    for (double r : rtts) { mn = std::min(mn, r); mx = std::max(mx, r); sum += r; }
    std::printf("%zu/%d replies, min/avg/max = %.1f/%.1f/%.1f ms\n", rtts.size(), count, mn, sum / (double)rtts.size(), mx);
    return 0;
}

static std::atomic<bool> g_stop{false};
static void on_sigint(int) { g_stop = true; }

static int cmd_serve(int argc, char** argv) {
    Args a(argc, argv, {"--port", "--motd", "--level", "--version", "--protocol", "--players", "--max"});
    int port = a.get_int("--port", 19132);
    std::string motd = a.get("--motd", "bedrockkit mock server");
    std::string level = a.get("--level", "Mock World");
    std::string version = a.get("--version", "1.21.0");
    int protocol = a.get_int("--protocol", 685), players = a.get_int("--players", 3), maxp = a.get_int("--max", 20);

    NetInit net;
    UdpSocket sock;
    sock.bind_any((uint16_t)port);
    std::mt19937_64 rng(std::random_device{}());
    uint64_t guid = rng() >> 1;
    std::signal(SIGINT, on_sigint);
    std::cout << "mock Bedrock server listening on UDP " << port << " (Ctrl+C to stop)\n";

    std::vector<uint8_t> buf;
    Address from;
    while (!g_stop) {
        if (!sock.recv_from(buf, from, 300)) continue;
        if (!is_ping(buf)) continue;
        std::string ad = "MCPE;" + motd + ";" + std::to_string(protocol) + ";" + version + ";" + std::to_string(players) + ";" +
                         std::to_string(maxp) + ";" + std::to_string(guid) + ";" + level + ";Survival;1;" + std::to_string(port) + ";" +
                         std::to_string(port) + ";";
        sock.send_to(from, build_pong(buf, guid, ad));
        std::cout << "answered ping from " << from.str() << "\n";
    }
    std::cout << "\nstopped\n";
    return 0;
}

// ---------------------------------------------------------------- main
static void usage() {
    std::cout <<
        "bedrockkit - tools for Minecraft: Bedrock Edition\n\n"
        "  bedrockkit ping <host> [port]        query a server (MOTD, players, version, latency)\n"
        "        [--count N] [--timeout ms] [--raw]\n"
        "  bedrockkit level <level.dat>         show a world's settings\n"
        "        [--dump] [--get Path] [--set Path=Value]... [--out file]\n"
        "  bedrockkit nbt <file>                dump any NBT file\n"
        "        [--big-endian] [--skip N]\n"
        "  bedrockkit sample [out.dat]          write a sample level.dat to experiment with\n"
        "  bedrockkit serve                     mock server that answers pings\n"
        "        [--port N] [--motd S] [--level S] [--version S] [--protocol N] [--players N] [--max N]\n";
}

int main(int argc, char** argv) {
    if (argc < 2 || !std::strcmp(argv[1], "--help") || !std::strcmp(argv[1], "help")) { usage(); return argc < 2 ? 1 : 0; }
    std::string cmd = argv[1];
    try {
        if (cmd == "ping") return cmd_ping(argc, argv);
        if (cmd == "serve") return cmd_serve(argc, argv);
        if (cmd == "level") return cmd_level(argc, argv);
        if (cmd == "nbt") return cmd_nbt(argc, argv);
        if (cmd == "sample") return cmd_sample(argc, argv);
        std::cerr << "unknown command: " << cmd << "\n\n";
        usage();
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
