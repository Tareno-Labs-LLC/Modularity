#include "MacFileDialogs.h"

#import <AppKit/AppKit.h>

namespace Modularity {
namespace {

NSURL* fileUrl(const std::filesystem::path& path) {
    std::error_code ec;
    const auto absolutePath = std::filesystem::absolute(path, ec);
    if (ec) return nil;
    NSString* name = [[NSFileManager defaultManager]
        stringWithFileSystemRepresentation:absolutePath.c_str()
        length:absolutePath.native().size()];
    return name ? [NSURL fileURLWithPath:name] : nil;
}

} // namespace

bool OpenMacPath(const std::filesystem::path& path) {
    @autoreleasepool {
        NSURL* url = fileUrl(path);
        return url && [[NSWorkspace sharedWorkspace] openURL:url];
    }
}

void RevealMacPath(const std::filesystem::path& path) {
    @autoreleasepool {
        NSURL* url = fileUrl(path);
        if (!url) return;
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) {
            [[NSWorkspace sharedWorkspace] openURL:url];
        } else {
            [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[url]];
        }
    }
}

std::optional<std::filesystem::path> ChooseMacPath(
    const std::filesystem::path& initialDirectory, bool chooseDirectory) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseDirectories = chooseDirectory;
        panel.canChooseFiles = !chooseDirectory;
        panel.allowsMultipleSelection = NO;
        if (!initialDirectory.empty()) {
            panel.directoryURL = fileUrl(initialDirectory);
        }
        if ([panel runModal] != NSModalResponseOK) return std::nullopt;
        const char* selected = panel.URL.fileSystemRepresentation;
        if (selected) return std::filesystem::path(selected);
        return std::nullopt;
    }
}

} // namespace Modularity
