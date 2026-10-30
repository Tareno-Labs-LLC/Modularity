#pragma once
#include <filesystem>
#include <string>
#include <vector>

// The native compiler and the JIT share generated wrappers and ABI checks.
// A C unit is compiled by TinyCC to an object and linked into the C++ JIT.
namespace EmbeddedScript {
struct Request {
    std::filesystem::path cppSource;
    std::filesystem::path cSource;
    std::filesystem::path toolchain;
    std::vector<std::string> arguments;
    std::vector<std::filesystem::path> includes;
    std::vector<std::string> defines;
};
bool Available();
bool Compile(const Request& request, const std::filesystem::path& artifact,
             std::string& log, std::string& error);
enum class LoadResult { NativeLibrary, Loaded, Failed };
LoadResult Load(const std::filesystem::path& artifact, void*& module, std::string& error);
void* Symbol(void* module, const char* name);
}
