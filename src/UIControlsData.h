#pragma once
#include <sstream>
#include <limits>
#include <string>
#include <vector>

namespace UIControlsData {
// Scene text encoding wraps this length-prefixed payload, so '=', '\\', UTF-8
// and line breaks cannot escape into other scene properties.
inline std::string Join(const std::vector<std::string>& values) {
    std::string result;
    for (const auto& value : values) {
        // Each entry is length-prefixed to preserve empty and multiline labels.
        result += std::to_string(value.size()) + ":" + value;
    }
    return result;
}
inline void Split(const std::string& payload, std::vector<std::string>& values) {
    std::vector<std::string> parsed;
    size_t cursor = 0;
    while (cursor < payload.size()) {
        size_t colon = payload.find(':', cursor);
        if (colon == std::string::npos || colon == cursor || colon - cursor > 10) return;
        size_t length = 0;
        for (size_t i = cursor; i < colon; ++i) {
            if (payload[i] < '0' || payload[i] > '9') return;
            size_t digit = static_cast<size_t>(payload[i] - '0');
            if (length > (std::numeric_limits<size_t>::max() - digit) / 10) return;
            length = length * 10 + digit;
        }
        cursor = colon + 1;
        if (length > payload.size() - cursor) return;
        parsed.emplace_back(payload, cursor, length);
        cursor += length;
    }
    values = std::move(parsed);
}
inline std::string Join(const std::vector<int>& values) {
    std::string result;
    for (int value : values) { if (!result.empty()) result += ','; result += std::to_string(value); }
    return result;
}
inline void Split(const std::string& payload, std::vector<int>& values) {
    std::vector<int> parsed;
    std::istringstream input(payload);
    int value;
    while (input >> value) {
        parsed.push_back(value);
        if (input.peek() == ',') input.get();
        else if (!input.eof()) return;
    }
    if (!input.eof()) return;
    values = std::move(parsed);
}
} // namespace UIControlsData
