#pragma once

#if !MODULARITY_RUNTIME_ONLY
#include "ThirdParty/ModuGUI/imgui.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <string>
#include <unordered_map>

namespace Modularity {

// A display-only overlay: project/preset colors never become wallpaper colors.
class EditorMatugenTheme {
public:
    bool enabled = false;
    std::string palettePath;
    float fadeSeconds = 0.35f;
    std::string status = "Matugen disabled";

    static std::filesystem::path defaultPalettePath() {
        if (const char* config = std::getenv("XDG_CONFIG_HOME")) {
            if (*config) return std::filesystem::path(config) / "matugen/colors.css";
        }
        if (const char* home = std::getenv("HOME")) {
            return std::filesystem::path(home) / ".config/matugen/colors.css";
        }
        return {};
    }

    std::filesystem::path resolvedPath() const {
        if (palettePath.empty()) return defaultPalettePath();
        if (palettePath.rfind("~/", 0) == 0) {
            if (const char* home = std::getenv("HOME")) {
                return std::filesystem::path(home) / palettePath.substr(2);
            }
        }
        return std::filesystem::path(palettePath);
    }

    ImGuiStyle baseStyle(const ImGuiStyle& live) const {
        ImGuiStyle result = live;
        if (applied) {
            for (int i = 0; i < ImGuiCol_COUNT; ++i) {
                // Preserve deliberate edits made since the overlay was drawn.
                if (equal(live.Colors[i], displayed[i])) result.Colors[i] = base[i];
            }
        }
        return result;
    }

    void restore(ImGuiStyle& style) {
        style = baseStyle(style);
        applied = false;
    }

    void refresh() { nextCheck = 0; }

    void update(ImGuiStyle& style, double now) {
        Colors current;
        std::copy_n(style.Colors, ImGuiCol_COUNT, current.begin());
        restore(style);
        if (enabled) poll(now);
        else status = "Matugen disabled";

        std::copy_n(style.Colors, ImGuiCol_COUNT, base.begin());
        Colors desired = base;
        if (enabled && !palette.empty()) applyPalette(desired);
        bool changed = !initialized;
        for (int i = 0; i < ImGuiCol_COUNT && !changed; ++i) changed = !equal(desired[i], target[i]);
        if (changed) {
            from = current;
            target = desired;
            started = now;
            initialized = true;
        }
        const float duration = std::isfinite(fadeSeconds) ? std::clamp(fadeSeconds, 0.0f, 5.0f) : .35f;
        float t = duration > 0 ? static_cast<float>(std::clamp((now - started) / duration, 0.0, 1.0)) : 1.0f;
        t = t * t * (3.0f - 2.0f * t);
        for (int i = 0; i < ImGuiCol_COUNT; ++i) {
            displayed[i] = mix(from[i], target[i], t);
            style.Colors[i] = displayed[i];
        }
        applied = true;
    }

private:
    using Colors = std::array<ImVec4, ImGuiCol_COUNT>;
    using Palette = std::unordered_map<std::string, ImVec4>;
    struct Result {
        std::filesystem::path path;
        std::filesystem::file_time_type stamp{};
        Palette colors;
        std::string error;
        bool unchanged = false;
        bool missing = false;
    };
    Colors base{}, displayed{}, from{}, target{};
    Palette palette;
    bool applied = false, initialized = false, tracked = false;
    double started = 0, nextCheck = 0;
    std::filesystem::path source;
    std::filesystem::file_time_type lastWrite{};
    std::future<Result> pending;

    static bool equal(const ImVec4& a, const ImVec4& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    }
    static ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
        return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                      a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
    }
    static bool hexColor(std::string value, ImVec4& result) {
        if (!value.empty() && value.back() == ';') value.pop_back();
        if (value.size() != 7 || value[0] != '#') return false;
        unsigned int rgb = 0;
        for (size_t i = 1; i < value.size(); ++i) {
            const char c = value[i];
            int digit = c >= '0' && c <= '9' ? c - '0' :
                        c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                        c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (digit < 0) return false;
            rgb = (rgb << 4) | static_cast<unsigned int>(digit);
        }
        result = ImVec4(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, 1.f);
        return true;
    }
    static Result readPalette(const std::filesystem::path& path, bool tracked,
                              std::filesystem::file_time_type previous) {
        Result result;
        result.path = path;
        std::error_code ec;
        if (path.empty() || !std::filesystem::is_regular_file(path, ec) || ec) {
            result.missing = true;
            result.error = "Palette unavailable; using project colors";
            return result;
        }
        result.stamp = std::filesystem::last_write_time(path, ec);
        if (ec) { result.error = "Cannot inspect palette"; return result; }
        if (tracked && result.stamp == previous) { result.unchanged = true; return result; }
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size > 256 * 1024) { result.error = "Palette exceeds 256 KiB or cannot be read"; return result; }
        std::ifstream file(path);
        if (!file) { result.error = "Cannot open palette"; return result; }
        std::string contents(static_cast<size_t>(size), '\0');
        file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (file.gcount() != static_cast<std::streamsize>(size)) {
            result.error = "Palette changed while reading; retrying";
            return result;
        }
        std::istringstream lines(contents);
        std::string line;
        while (std::getline(lines, line)) {
            std::istringstream fields(line);
            std::string directive, name, value;
            if (!(fields >> directive >> name >> value) || directive != "@define-color") continue;
            ImVec4 color;
            if (hexColor(value, color)) result.colors[name] = color;
        }
        for (const char* required : {"theme_bg", "theme_panel", "theme_text", "theme_accent"}) {
            if (!result.colors.count(required)) {
                result.colors.clear();
                result.error = std::string("Palette needs @define-color ") + required + " #rrggbb;";
                return result;
            }
        }
        return result;
    }

    void poll(double now) {
        if (pending.valid() && pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                Result result = pending.get();
                if (result.path == resolvedPath()) {
                    if (result.missing) { palette.clear(); tracked = false; }
                    if (!result.error.empty()) status = result.error;
                    else {
                        if (!result.unchanged) {
                            palette = std::move(result.colors);
                            lastWrite = result.stamp;
                            tracked = true;
                        }
                        status = "Following Matugen palette";
                    }
                }
            } catch (const std::exception& error) { status = error.what(); }
        }
        if (now < nextCheck || pending.valid()) return;
        nextCheck = now + 1.0;
        const auto path = resolvedPath();
        if (path != source) {
            source = path;
            tracked = false;
            palette.clear();
        }
        try {
            pending = std::async(std::launch::async, readPalette, path, tracked, lastWrite);
        } catch (const std::exception& error) { status = error.what(); }
    }

    void applyPalette(Colors& colors) const {
        auto get = [&](const char* name, const char* fallback) {
            const auto found = palette.find(name);
            return found != palette.end() ? found->second : palette.at(fallback);
        };
        const ImVec4 bg = get("theme_bg", "theme_bg"), panel = get("theme_panel", "theme_panel");
        const ImVec4 text = get("theme_text", "theme_text"), accent = get("theme_accent", "theme_accent");
        const ImVec4 button = get("theme_button", "theme_panel"), hover = get("theme_hover", "theme_accent");
        const ImVec4 selected = get("theme_select", "theme_accent"), edge = get("theme_edge", "theme_panel");
        auto set = [&](ImGuiCol slot, ImVec4 color) { color.w = base[slot].w; colors[slot] = color; };
        set(ImGuiCol_Text, text);
        set(ImGuiCol_TextDisabled, get("theme_muted", "theme_text"));
        for (auto slot : {ImGuiCol_WindowBg, ImGuiCol_DockingEmptyBg, ImGuiCol_TitleBg}) set(slot, bg);
        for (auto slot : {ImGuiCol_ChildBg, ImGuiCol_PopupBg, ImGuiCol_MenuBarBg, ImGuiCol_ScrollbarBg,
                          ImGuiCol_TableHeaderBg, ImGuiCol_TitleBgCollapsed, ImGuiCol_TabDimmed}) set(slot, panel);
        for (auto slot : {ImGuiCol_FrameBg, ImGuiCol_Button, ImGuiCol_Header, ImGuiCol_Tab,
                          ImGuiCol_ScrollbarGrab}) set(slot, button);
        for (auto slot : {ImGuiCol_FrameBgHovered, ImGuiCol_ButtonHovered, ImGuiCol_HeaderHovered,
                          ImGuiCol_TabHovered, ImGuiCol_ScrollbarGrabHovered, ImGuiCol_SeparatorHovered,
                          ImGuiCol_ResizeGripHovered}) set(slot, hover);
        for (auto slot : {ImGuiCol_FrameBgActive, ImGuiCol_ButtonActive, ImGuiCol_HeaderActive,
                          ImGuiCol_TabSelected, ImGuiCol_TabDimmedSelected, ImGuiCol_TitleBgActive,
                          ImGuiCol_ScrollbarGrabActive, ImGuiCol_TextSelectedBg}) set(slot, selected);
        for (auto slot : {ImGuiCol_CheckMark, ImGuiCol_SliderGrab, ImGuiCol_SliderGrabActive,
                          ImGuiCol_SeparatorActive, ImGuiCol_ResizeGripActive, ImGuiCol_NavCursor,
                          ImGuiCol_DockingPreview, ImGuiCol_TabSelectedOverline,
                          ImGuiCol_TabDimmedSelectedOverline, ImGuiCol_PlotLinesHovered,
                          ImGuiCol_PlotHistogramHovered}) set(slot, accent);
        for (auto slot : {ImGuiCol_Border, ImGuiCol_Separator, ImGuiCol_TableBorderStrong,
                          ImGuiCol_TableBorderLight, ImGuiCol_ResizeGrip}) set(slot, edge);
    }
};
} // namespace Modularity
#endif
