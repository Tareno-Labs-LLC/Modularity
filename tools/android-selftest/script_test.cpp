#include "ScriptCompiler.h"
#include "AndroidScript.h"
#include <cstdio>
#include <fstream>

// Generate an import library for the control case, just as the desktop host does.
extern "C" __declspec(dllexport) void AndroidScriptTestHost() {}
static void write(const fs::path& path, const char* text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    const fs::path repo = fs::absolute(argv[1]), out = fs::absolute(argv[3]);
    ScriptBuildConfig config;
    config.scriptsDir = out / "sources";
    config.outDir = out / "binaries";
    for (const char* path : {"src", "include", "src/ThirdParty", "src/ThirdParty/glad", "src/ThirdParty/glm", "src/ThirdParty/ModuGUI", "src/ThirdParty/ModuGUI/backends", "src/ThirdParty/glfw/include", "src/ThirdParty/openxr/include"}) config.includeDirs.push_back(repo / path);
    config.defines = {"MODULARITY_RUNTIME_ONLY=1", "MODULARITY_PLAYER=1", "MODULARITY_OPENGL_ES=1", "MODULARITY_USE_MONO=0", "MODULARITY_ENABLE_ASSIMP=0", "MODULARITY_HAS_FFMPEG=0", "MODULARITY_HAS_OPENXR=1"};
    const fs::path cpp = config.scriptsDir / "native.cpp";
    write(cpp, "#include \"ScriptRuntime.h\"\nvoid Begin(ScriptContext& context) { context.SetObjectEnabled(true); }\n");
    ScriptCompiler compiler;
    ScriptBuildCommands commands;
    std::string error;
    config.defines[2] = "MODULARITY_OPENGL_ES=0";
    if (!compiler.makeCommands(config, cpp, commands, error) || commands.binaryPath.extension() != ".dll" || commands.buildSignature.find("/std:c++23preview") == std::string::npos) {
        std::fprintf(stderr, "Native Windows control failed: %s\n", error.c_str()); return 1;
    }
    std::puts("[PASS] native Windows script commands still use MSVC and DLLs");
    config.defines[2] = "MODULARITY_OPENGL_ES=1";
    config.crossCompilerDriver = argv[2];
    config.extraLinkFlags = {"-Wl,-z,max-page-size=16384"};
    const fs::path modu = config.scriptsDir / "Runtime/smoke.moducpp";
    write(modu, "add ModuCPP;\npublic class Smoke : ModuNode { public float speed = 1.0f; void TickUpdate() {} }\n");
    const fs::path c = config.scriptsDir / "c_api.c";
    write(c, "typedef struct ModuScriptContext ModuScriptContext;\n#ifdef __cplusplus\nextern \"C\" {\n#endif\nvoid Modu_Begin(ModuScriptContext* context) { (void)context; }\n#ifdef __cplusplus\n}\n#endif\n");
    for (const auto& source : {cpp, modu, c}) {
        if (!compiler.makeCommands(config, source, commands, error)) {
            std::fprintf(stderr, "%s\n", error.c_str()); return 1;
        }
        if (commands.binaryPath.extension() != ".so" || commands.objectPath.extension() != ".o" || commands.compile.find("-std=c++23") == std::string::npos || commands.compile.find("/std:") != std::string::npos || commands.link.find(".lib\"") != std::string::npos) {
            std::fprintf(stderr, "Wrong Android commands: %s\n%s\n", commands.compile.c_str(), commands.link.c_str()); return 1;
        }
        ScriptCompileOutput result;
        const auto relative = moduAndroidScriptRelativeBinary(commands.binaryPath, config.outDir);
        if (relative.empty() || relative.generic_string().find(".staging") != std::string::npos ||
            moduAndroidScriptSoname(relative) == moduAndroidScriptSoname(commands.linkBinaryPath.lexically_relative(config.outDir))) {
            std::fprintf(stderr, "Temporary staging paths must not enter packaged script names\n"); return 1;
        }
        if (!compiler.compile(commands, result, error)) {
            std::fprintf(stderr, "%s\n%s\n%s\n", error.c_str(), result.compileLog.c_str(), result.linkLog.c_str()); return 1;
        }
        std::ifstream file(result.producedBinaryPath, std::ios::binary);
        unsigned char header[20]{}; file.read(reinterpret_cast<char*>(header), sizeof(header));
        if (header[0] != 0x7f || header[1] != 'E' || header[2] != 'L' || header[3] != 'F' || header[4] != 2 || header[18] != 183 || header[19] != 0) {
            std::fprintf(stderr, "Expected ARM64 ELF library: %s\n", result.producedBinaryPath.string().c_str()); return 1;
        }
        std::printf("[PASS] %s compiled and linked to an ARM64 ELF library on Windows\n", source.filename().string().c_str());
    }
    return 0;
}
