#include "Platform/ExecutablePath.h"
#include "ModularityLspServer.h"
#include <filesystem>

int main(int argc, char** argv) {
    if (auto exeDir = Modularity::CurrentExecutablePath().parent_path(); !exeDir.empty()) {
        std::error_code ec;
        std::filesystem::current_path(exeDir, ec);
    }
    ModularityLspServer lspServer;
    return lspServer.run(argc, argv);
}
