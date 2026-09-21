#pragma once

// Crash-safe replacement for files a user cannot regenerate: scenes and project.modu.
//
// std::ofstream(path) truncates the old file first and leaves the new bytes in the OS cache.
// If the machine goes down before that cache is flushed, NTFS can keep the new file length
// without the data - the file comes back the right size and entirely zero bytes, and the
// previous version is already gone. A save shortly before a crash wiped a 2.6 MB scene and
// project.modu exactly this way.
//
// WriteFileDurably writes a sibling temp file, forces it to disk, then renames it over the
// target. After a crash the target holds either the old contents or the new ones.

#include <filesystem>
#include <string>
#include <string_view>

namespace Modularity {

enum class DurableFileMode {
    Binary,
    // "\n" becomes "\r\n" on Windows, which is what std::ofstream in text mode wrote, so files
    // saved through here stay byte-identical to the ones saved before.
    Text,
};

bool WriteFileDurably(const std::filesystem::path& path,
                      std::string_view contents,
                      std::string& error,
                      DurableFileMode mode = DurableFileMode::Binary);

// The same guarantee for a file that gets rewritten in bursts. A flush costs ~30 ms (p95 ~85 ms)
// on a typical Windows SSD, and project.modu is saved on every frame a Project Settings slider
// is dragged or a script sets gravity. A save to a path that has been quiet for a second writes
// synchronously, so one-off callers (create a project, then load it) see the file on return.
// Saves that follow within that second are handed to a background thread and coalesced, so
// only the newest contents are written and the frame never waits on the disk.
// Failures are logged to stderr.
void WriteFileDurablyCoalesced(const std::filesystem::path& path,
                               std::string contents,
                               DurableFileMode mode = DurableFileMode::Binary);

// Blocks until coalesced writes have landed. Call before reading a file that may still be
// queued (Project::load does), and at shutdown.
void FlushDurableWrites();

// Scenes and project.modu are line-based text, so a NUL byte anywhere means the file did not
// survive a write (the crash signature above). Parsing such a file "succeeds" with nothing in
// it, so check before trusting the result.
bool FileContainsNulBytes(const std::filesystem::path& path);

inline constexpr const char* kDamagedProjectFileMessage =
    "project.modu is damaged (it is full of zero bytes, which is what a crash right after a save "
    "leaves behind). Restore it from a backup - an exported build's _runtime_stage/project.modu "
    "is a copy.";
inline constexpr const char* kDamagedSceneFileMessage =
    "is damaged (it is full of zero bytes, which is what a crash right after a save leaves "
    "behind). Restore it from a backup - an exported build's _runtime_stage/Assets/Scenes, or the "
    "Assets/Scenes entry inside its content.modbundle, has a copy.";

} // namespace Modularity
