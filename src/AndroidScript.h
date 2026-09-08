#pragma once
#include "Common.h"
#include <cctype>
#include <cstdint>
#include <string>
inline std::string moduAndroidScriptSoname(const fs::path& binaryRelativeToOutDir) {
    std::string raw = binaryRelativeToOutDir.generic_string();
    for (const char* ext : {".so", ".dll", ".dylib"}) {
        const std::string e(ext);
        if (raw.size() >= e.size() && raw.compare(raw.size() - e.size(), e.size(), e) == 0) {raw.resize(raw.size() - e.size()); break;}
    }
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : raw) {hash ^= c; hash *= 1099511628211ull;}
    std::string sanitized;
    sanitized.reserve(raw.size());
    for (unsigned char c : raw) {sanitized += std::isalnum(c) ? static_cast<char>(c) : '_';}
    if (sanitized.size() > 80) sanitized.resize(80);
    static const char kHex[] = "0123456789abcdef";
    std::string hashHex;
    for (int shift = 28; shift >= 0; shift -= 4) {hashHex += kHex[(hash >> shift) & 0xF];}
    return "libmodu_" + sanitized + "_" + hashHex + ".so";
}
