#include "EditorMatugenTheme.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
using Modularity::EditorMatugenTheme;

static bool near(float a, float b) { return std::abs(a - b) < .001f; }

int main() {
    const auto dir = fs::temp_directory_path() /
        ("modularity-matugen-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    struct Cleanup { fs::path dir; ~Cleanup() { fs::remove_all(dir); } } cleanup{dir};
    const auto path = dir / "colors.css";
    auto write = [&](const char* accent) {
        std::ofstream file(dir / "next.css");
        file << "/* Generated palette */\n"
             << "@define-color theme_bg #102030;\n"
             << "@define-color theme_panel #203040;\n"
             << "@define-color theme_text #e0d0c0;\n"
             << "@define-color theme_accent " << accent << ";\n"
             << "@define-color theme_select_top mix(@theme_panel, @theme_accent, 0.16);\n";
        file.close();
        std::error_code ec;
        fs::remove(path, ec);
        fs::rename(dir / "next.css", path);
    };
    write("#ff0000");
    ImGuiStyle style;
    for (auto& color : style.Colors) color = ImVec4(.2f, .3f, .4f, .8f);
    const ImGuiStyle original = style;
    EditorMatugenTheme theme;
    double now = 0;
    theme.update(style, now);
    assert(near(style.Colors[ImGuiCol_CheckMark].x, .2f));
    theme.palettePath = path.string();
    theme.enabled = true;
    auto await = [&](auto condition) {
        for (int i = 0; i < 3000; ++i) {
            now += .01;
            theme.update(style, now);
            if (condition()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::cerr << theme.status << '\n';
        assert(false && "palette update timed out");
    };
    await([&] { return theme.status == "Following Matugen palette"; });
    const double fadeStart = now;
    assert(near(style.Colors[ImGuiCol_CheckMark].x, .2f));
    now = fadeStart + .175;
    theme.update(style, now);
    assert(near(style.Colors[ImGuiCol_CheckMark].x, .6f));
    assert(near(theme.baseStyle(style).Colors[ImGuiCol_CheckMark].x, .2f));
    assert(near(style.Colors[ImGuiCol_CheckMark].w, .8f));
    now += .175;
    theme.update(style, now);
    assert(near(style.Colors[ImGuiCol_CheckMark].x, 1));
    assert(style.WindowRounding == original.WindowRounding);

    // Atomic replacement must start from the currently displayed color.
    write("#0000ff");
    theme.refresh();
    await([&] { return style.Colors[ImGuiCol_CheckMark].z > .01f; });
    now += 1;
    theme.update(style, now);
    assert(near(style.Colors[ImGuiCol_CheckMark].z, 1));
    assert(near(style.Colors[ImGuiCol_CheckMark].x, 0));

    // A partially written/invalid file must never replace the last valid palette.
    { std::ofstream file(path); file << "@define-color theme_bg #nope;\n"; }
    theme.refresh();
    await([&] { return theme.status.find("Palette needs") != std::string::npos; });
    assert(near(style.Colors[ImGuiCol_CheckMark].z, 1));

    // The toggle fades back to the original project colors, including transparency.
    theme.enabled = false;
    theme.update(style, ++now);
    now += 1;
    theme.update(style, now);
    assert(near(style.Colors[ImGuiCol_CheckMark].x, .2f));
    assert(near(style.Colors[ImGuiCol_CheckMark].z, .4f));
    assert(near(style.Colors[ImGuiCol_CheckMark].w, .8f));

    // Missing files degrade to project colors; external editor color edits survive.
    fs::remove(path);
    theme.enabled = true;
    theme.refresh();
    await([&] { return theme.status.find("unavailable") != std::string::npos; });
    now += 1;
    theme.update(style, now);
    assert(near(style.Colors[ImGuiCol_CheckMark].x, .2f));
    style.Colors[ImGuiCol_CheckMark].x = .7f;
    assert(near(theme.baseStyle(style).Colors[ImGuiCol_CheckMark].x, .7f));
    theme.restore(style);
    assert(near(style.Colors[ImGuiCol_CheckMark].x, .7f));
    std::cout << "PASS: opt-in palette, fade interpolation, original colors, alpha/layout, atomic replacement, malformed/missing files, toggle restore, live edits\n";
}
