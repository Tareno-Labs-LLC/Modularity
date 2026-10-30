#include "EmbeddedScript.h"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "embedded-probe <extracted-bundle> <scratch-directory>\n"; return 2; }
    const fs::path root = fs::absolute(argv[1]), scratch = fs::absolute(argv[2]);
    fs::create_directories(scratch);
    EmbeddedScript::Request request;
    request.toolchain = root;
    request.cppSource = scratch / "bridge.cpp";
    request.cSource = scratch / "script.c";
    request.arguments = {"-std=c++23", "-target", "aarch64-linux-android26", "--sysroot=" + (root / "sysroot").string(),
        "-nostdinc++", "-isystem", (root / "sysroot/usr/include/c++/v1").string()};
    request.includes = {root / "cling/include"};
    for (const auto& entry : fs::directory_iterator(root / "cling/lib/clang")) {
        if (fs::exists(entry.path() / "include/stddef.h")) {
            request.arguments.push_back("-resource-dir"); request.arguments.push_back(entry.path().string());
        }
    }
    std::ofstream(request.cppSource) <<
        "struct Context { int value; };\n"
        "extern \"C\" {\n"
        "void Modu_Begin(Context*);\n"
        "int Modu_GetValue(Context* ctx) { return ctx->value; }\n"
        "int Modularity_ScriptAbiVersion() { return 62; }\n"
        "int Script_Begin(Context* ctx) { Modu_Begin(ctx); return ctx->value; }\n}\n";
    auto writeC = [&](int increment) {
        std::ofstream(request.cSource) <<
            "#ifdef __cplusplus\n#error Expected real C compilation\n#endif\n"
            "struct Context { int value; };\n"
            "extern int Modu_GetValue(struct Context*);\n"
            "void Modu_Begin(struct Context* ctx) { ctx->value = Modu_GetValue(ctx) + " << increment << "; }\n";
    };
    std::string log, error;
    auto compile = [&]() -> void* {
        if (!EmbeddedScript::Compile(request, scratch / "script.so", log, error)) return nullptr;
        void* module = nullptr;
        if (EmbeddedScript::Load(scratch / "script.so", module, error) != EmbeddedScript::LoadResult::Loaded) return nullptr;
        return module;
    };
    struct Context { int value; } context{35};
    using Entry = int (*)(Context*);
    writeC(7);
    void* first = compile();
    auto oldEntry = reinterpret_cast<Entry>(EmbeddedScript::Symbol(first, "Script_Begin"));
    if (!oldEntry || oldEntry(&context) != 42) { std::cerr << error << '\n' << log; return 1; }
    writeC(70);
    void* second = compile();
    auto newEntry = reinterpret_cast<Entry>(EmbeddedScript::Symbol(second, "Script_Begin"));
    context.value = 35;
    if (!newEntry || newEntry(&context) != 105) { std::cerr << error << '\n' << log; return 1; }
    context.value = 35;
    if (oldEntry(&context) != 42) return 1;
    std::cout << "[PASS] TinyCC C and Cling C++ linked through the context bridge\n"
                 "[PASS] Reload retained executable code referenced by the old callback\n";
    return 0;
}
