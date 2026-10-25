// nbt.hpp - NBT reader/writer/printer. Bedrock uses little-endian NBT; Java uses big-endian.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bk {

enum class TagType : uint8_t {
    End = 0, Byte, Short, Int, Long, Float, Double, ByteArray, String, List, Compound, IntArray, LongArray
};

struct Tag {
    TagType type = TagType::End;
    int64_t i = 0;                  // Byte / Short / Int / Long
    double f = 0;                   // Float / Double
    std::string s;                  // String
    std::vector<int64_t> arr;       // ByteArray / IntArray / LongArray
    TagType elem = TagType::End;    // List element type
    std::vector<Tag> list;          // List items
    std::vector<std::string> keys;  // Compound keys (insertion order preserved)
    std::vector<Tag> vals;          // Compound values

    static Tag integer(TagType t, int64_t v) { Tag x; x.type = t; x.i = v; return x; }
    static Tag floating(TagType t, double v) { Tag x; x.type = t; x.f = v; return x; }
    static Tag string(std::string v) { Tag x; x.type = TagType::String; x.s = std::move(v); return x; }
    static Tag compound() { Tag x; x.type = TagType::Compound; return x; }
    static Tag make_list(TagType elem) { Tag x; x.type = TagType::List; x.elem = elem; return x; }

    Tag* find(const std::string& k) {
        for (size_t n = 0; n < keys.size(); ++n) if (keys[n] == k) return &vals[n];
        return nullptr;
    }
    const Tag* find(const std::string& k) const { return const_cast<Tag*>(this)->find(k); }
    Tag& set(const std::string& k, Tag v) {
        if (Tag* t = find(k)) { *t = std::move(v); return *t; }
        keys.push_back(k); vals.push_back(std::move(v));
        return vals.back();
    }
};

inline bool is_scalar(TagType t) { return t >= TagType::Byte && t <= TagType::Double; }

// ---------------------------------------------------------------- reader
class Reader {
public:
    Reader(const uint8_t* data, size_t size, bool little_endian)
        : p_(data), end_(data + size), le_(little_endian) {}

    size_t remaining() const { return (size_t)(end_ - p_); }

    // Parses one named root tag: [type][name][payload]
    std::pair<std::string, Tag> read_root() {
        auto ty = (TagType)u(1);
        if (ty == TagType::End) throw std::runtime_error("NBT: empty root");
        check_type(ty);
        std::string name = read_string();
        return {name, read_payload(ty, 0)};
    }

private:
    void need(size_t n) const { if (n > remaining()) throw std::runtime_error("NBT: unexpected end of data"); }
    static void check_type(TagType t) { if ((uint8_t)t > 12) throw std::runtime_error("NBT: invalid tag type"); }

    uint64_t u(int bytes) {
        need((size_t)bytes);
        uint64_t v = 0;
        if (le_) for (int k = bytes - 1; k >= 0; --k) v = (v << 8) | p_[k];
        else for (int k = 0; k < bytes; ++k) v = (v << 8) | p_[k];
        p_ += bytes;
        return v;
    }
    std::string read_string() {
        size_t n = (size_t)u(2);
        need(n);
        std::string s((const char*)p_, n);
        p_ += n;
        return s;
    }
    size_t read_count(size_t elem_size) {
        int32_t n = (int32_t)u(4);
        if (n < 0 || (size_t)n > remaining() / elem_size) throw std::runtime_error("NBT: invalid length");
        return (size_t)n;
    }

    Tag read_payload(TagType ty, int depth) {
        if (depth > 512) throw std::runtime_error("NBT: nesting too deep");
        Tag t;
        t.type = ty;
        switch (ty) {
            case TagType::Byte:  t.i = (int8_t)u(1); break;
            case TagType::Short: t.i = (int16_t)u(2); break;
            case TagType::Int:   t.i = (int32_t)u(4); break;
            case TagType::Long:  t.i = (int64_t)u(8); break;
            case TagType::Float: { uint32_t b = (uint32_t)u(4); float f; std::memcpy(&f, &b, 4); t.f = f; break; }
            case TagType::Double: { uint64_t b = u(8); double d; std::memcpy(&d, &b, 8); t.f = d; break; }
            case TagType::ByteArray: { size_t n = read_count(1); for (size_t k = 0; k < n; ++k) t.arr.push_back((int8_t)u(1)); break; }
            case TagType::IntArray:  { size_t n = read_count(4); for (size_t k = 0; k < n; ++k) t.arr.push_back((int32_t)u(4)); break; }
            case TagType::LongArray: { size_t n = read_count(8); for (size_t k = 0; k < n; ++k) t.arr.push_back((int64_t)u(8)); break; }
            case TagType::String: t.s = read_string(); break;
            case TagType::List: {
                t.elem = (TagType)u(1);
                check_type(t.elem);
                int32_t n = (int32_t)u(4);
                if (n < 0) throw std::runtime_error("NBT: invalid list length");
                if (t.elem == TagType::End) { if (n > 0) throw std::runtime_error("NBT: list of End"); break; }
                if ((size_t)n > remaining()) throw std::runtime_error("NBT: invalid list length");
                for (int32_t k = 0; k < n; ++k) t.list.push_back(read_payload(t.elem, depth + 1));
                break;
            }
            case TagType::Compound:
                for (;;) {
                    auto ct = (TagType)u(1);
                    if (ct == TagType::End) break;
                    check_type(ct);
                    std::string key = read_string();
                    t.keys.push_back(std::move(key));
                    t.vals.push_back(read_payload(ct, depth + 1));
                }
                break;
            case TagType::End: throw std::runtime_error("NBT: unexpected End tag");
        }
        return t;
    }

    const uint8_t* p_;
    const uint8_t* end_;
    bool le_;
};

// ---------------------------------------------------------------- writer
class Writer {
public:
    explicit Writer(bool little_endian) : le_(little_endian) {}

    std::vector<uint8_t> write_root(const std::string& name, const Tag& t) {
        out_.clear();
        u((uint64_t)t.type, 1);
        str(name);
        payload(t);
        return out_;
    }

private:
    void u(uint64_t v, int bytes) {
        for (int k = 0; k < bytes; ++k) {
            int shift = le_ ? 8 * k : 8 * (bytes - 1 - k);
            out_.push_back((uint8_t)(v >> shift));
        }
    }
    void str(const std::string& s) {
        if (s.size() > 65535) throw std::runtime_error("NBT: string too long");
        u(s.size(), 2);
        out_.insert(out_.end(), s.begin(), s.end());
    }
    void payload(const Tag& t) {
        switch (t.type) {
            case TagType::Byte:  u((uint64_t)t.i, 1); break;
            case TagType::Short: u((uint64_t)t.i, 2); break;
            case TagType::Int:   u((uint64_t)t.i, 4); break;
            case TagType::Long:  u((uint64_t)t.i, 8); break;
            case TagType::Float: { float f = (float)t.f; uint32_t b; std::memcpy(&b, &f, 4); u(b, 4); break; }
            case TagType::Double: { double d = t.f; uint64_t b; std::memcpy(&b, &d, 8); u(b, 8); break; }
            case TagType::ByteArray: u(t.arr.size(), 4); for (auto v : t.arr) u((uint64_t)v, 1); break;
            case TagType::IntArray:  u(t.arr.size(), 4); for (auto v : t.arr) u((uint64_t)v, 4); break;
            case TagType::LongArray: u(t.arr.size(), 4); for (auto v : t.arr) u((uint64_t)v, 8); break;
            case TagType::String: str(t.s); break;
            case TagType::List:
                u((uint64_t)t.elem, 1);
                u(t.list.size(), 4);
                for (auto& e : t.list) payload(e);
                break;
            case TagType::Compound:
                for (size_t k = 0; k < t.keys.size(); ++k) {
                    u((uint64_t)t.vals[k].type, 1);
                    str(t.keys[k]);
                    payload(t.vals[k]);
                }
                u(0, 1);
                break;
            case TagType::End: break;
        }
    }
    std::vector<uint8_t> out_;
    bool le_;
};

// ---------------------------------------------------------------- SNBT printer
inline std::string fmt_real(double v, bool is_double) {
    char buf[64];
    for (int p = 1; p <= (is_double ? 17 : 9); ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, v);
        if (is_double ? std::strtod(buf, nullptr) == v : std::strtof(buf, nullptr) == (float)v) break;
    }
    std::string s = buf;
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

inline std::string quote(const std::string& s) {
    std::string r = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { r += '\\'; r += c; }
        else if (c == '\n') r += "\\n";
        else r += c;
    }
    return r + "\"";
}

inline std::string scalar_str(const Tag& t) {
    switch (t.type) {
        case TagType::Byte:   return std::to_string(t.i) + "b";
        case TagType::Short:  return std::to_string(t.i) + "s";
        case TagType::Int:    return std::to_string(t.i);
        case TagType::Long:   return std::to_string(t.i) + "L";
        case TagType::Float:  return fmt_real(t.f, false) + "f";
        case TagType::Double: return fmt_real(t.f, true) + "d";
        case TagType::String: return quote(t.s);
        default: return "?";
    }
}

inline std::string key_str(const std::string& k) {
    bool plain = !k.empty();
    for (char c : k) if (!(std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '+' || c == '-')) plain = false;
    return plain ? k : quote(k);
}

inline void snbt(std::ostream& o, const Tag& t, int lvl = 0) {
    auto pad = [&](int l) { o << std::string((size_t)l * 2, ' '); };
    switch (t.type) {
        case TagType::ByteArray:
        case TagType::IntArray:
        case TagType::LongArray: {
            const char* pre = t.type == TagType::ByteArray ? "B;" : t.type == TagType::IntArray ? "I;" : "L;";
            const char* suf = t.type == TagType::ByteArray ? "b" : t.type == TagType::LongArray ? "L" : "";
            o << "[" << pre;
            size_t shown = std::min<size_t>(t.arr.size(), 16);
            for (size_t k = 0; k < shown; ++k) o << (k ? ", " : " ") << t.arr[k] << suf;
            if (t.arr.size() > shown) o << ", ... (" << t.arr.size() << " total)";
            o << "]";
            break;
        }
        case TagType::List:
            if (t.list.empty()) { o << "[]"; break; }
            if ((is_scalar(t.elem) || t.elem == TagType::String) && t.list.size() <= 12) {
                o << "[";
                for (size_t k = 0; k < t.list.size(); ++k) o << (k ? ", " : "") << scalar_str(t.list[k]);
                o << "]";
            } else {
                o << "[\n";
                for (size_t k = 0; k < t.list.size(); ++k) {
                    pad(lvl + 1); snbt(o, t.list[k], lvl + 1);
                    o << (k + 1 < t.list.size() ? ",\n" : "\n");
                }
                pad(lvl); o << "]";
            }
            break;
        case TagType::Compound:
            if (t.keys.empty()) { o << "{}"; break; }
            o << "{\n";
            for (size_t k = 0; k < t.keys.size(); ++k) {
                pad(lvl + 1); o << key_str(t.keys[k]) << ": "; snbt(o, t.vals[k], lvl + 1);
                o << (k + 1 < t.keys.size() ? ",\n" : "\n");
            }
            pad(lvl); o << "}";
            break;
        default: o << scalar_str(t);
    }
}

// ---------------------------------------------------------------- path helpers
// Resolve "A.B.C" through nested compounds.
inline Tag* resolve(Tag& root, const std::string& path) {
    Tag* cur = &root;
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t dot = path.find('.', pos);
        std::string part = path.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
        if (cur->type != TagType::Compound) return nullptr;
        cur = cur->find(part);
        if (!cur) return nullptr;
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    return cur;
}

// Overwrite a scalar/string tag from user text, keeping its existing type.
inline void assign_from_string(Tag& t, const std::string& text) {
    auto range = [&](int64_t lo, int64_t hi) {
        int64_t v;
        if (text == "true") v = 1;
        else if (text == "false") v = 0;
        else {
            char* end = nullptr;
            v = std::strtoll(text.c_str(), &end, 10);
            if (text.empty() || *end) throw std::runtime_error("not an integer: " + text);
        }
        if (v < lo || v > hi) throw std::runtime_error("value out of range for this tag type: " + text);
        t.i = v;
    };
    switch (t.type) {
        case TagType::Byte:   range(-128, 127); break;
        case TagType::Short:  range(-32768, 32767); break;
        case TagType::Int:    range(INT32_MIN, INT32_MAX); break;
        case TagType::Long:   range(INT64_MIN, INT64_MAX); break;
        case TagType::Float:
        case TagType::Double: {
            char* end = nullptr;
            double v = std::strtod(text.c_str(), &end);
            if (text.empty() || *end) throw std::runtime_error("not a number: " + text);
            t.f = v;
            break;
        }
        case TagType::String: t.s = text; break;
        default: throw std::runtime_error("only numbers, bytes and strings can be set");
    }
}

}  // namespace bk
