#include "ProjectManager.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void Check(const std::filesystem::path& file, const char* setting, int expected) {
    {
        std::ofstream output(file);
        output << "name=Frame limit regression\nrenderer=OpenGL\ngraphicsVSync=0\n" << setting;
    }
    Project project;
    Require(project.load(file), "Load frame limit fixture");
    Require(project.graphicsSettings.targetFps == expected, "Loaded frame limit changed");
    project.saveProjectFile();
    Project reloaded;
    Require(reloaded.load(file), "Reload frame limit fixture");
    Require(reloaded.graphicsSettings.targetFps == expected, "Frame limit did not survive saving");
    Require(!reloaded.graphicsSettings.vsync, "Frame limit changed VSync");
}
} // namespace

int main(int argc, char** argv) {
    try {
        Require(argc == 2, "Pass a temporary output directory");
        const auto output = std::filesystem::absolute(argv[1]);
        std::filesystem::create_directories(output);
#ifndef __ANDROID__
        Require(ProjectGraphicsSettings{}.targetFps == 0, "Desktop defaults must be uncapped");
#endif
        const auto file = output / "project.modu";
        Check(file, "graphicsTargetFps=0\n", 0);
        Check(file, "graphicsTargetFps=144\n", 144);
        Check(file, "graphicsTargetFps=300\n", 300);
        Check(file, "graphicsTargetFps=500\n", 500);
        Check(file, "graphicsTargetFps=-1\n", 0);
#ifndef __ANDROID__
        Check(file, "", 0);
#endif
        std::cout << "PASS: uncapped desktop defaults and zero/explicit frame limit round trips\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
