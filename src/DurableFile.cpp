#include "DurableFile.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef APIENTRY
#undef APIENTRY
#endif
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace Modularity {
namespace {

fs::path TempPathFor(const fs::path& path) {
    fs::path temp = path;
    temp += ".saving";
    return temp;
}

#ifdef _WIN32

std::string ToWindowsText(std::string_view text) {
    std::string out;
    out.reserve(text.size() + static_cast<size_t>(std::count(text.begin(), text.end(), '\n')));
    for (char ch : text) {
        if (ch == '\n') out.push_back('\r');
        out.push_back(ch);
    }
    return out;
}

std::string DescribeWin32Error(const std::string& what, const fs::path& path, DWORD code) {
    return what + ": " + path.string() + " (" + std::system_category().message(static_cast<int>(code)) + ")";
}

bool WriteAndFlush(const fs::path& path, std::string_view contents, std::string& error) {
    // Same sharing std::ofstream used, so a reader holding the file open does not block the save.
    HANDLE file = CreateFileW(path.wstring().c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = DescribeWin32Error("Failed to open for writing", path, GetLastError());
        return false;
    }

    size_t offset = 0;
    while (offset < contents.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(contents.size() - offset, size_t{1} << 30));
        DWORD written = 0;
        if (!WriteFile(file, contents.data() + offset, chunk, &written, nullptr) || written == 0) {
            error = DescribeWin32Error("Failed to write", path, GetLastError());
            CloseHandle(file);
            return false;
        }
        offset += written;
    }

    if (!FlushFileBuffers(file)) {
        error = DescribeWin32Error("Failed to flush to disk", path, GetLastError());
        CloseHandle(file);
        return false;
    }
    CloseHandle(file);
    return true;
}

bool ReplaceWithTemp(const fs::path& temp, const fs::path& path, DWORD& lastError) {
    // WRITE_THROUGH: do not return until the rename itself is on disk.
    // Antivirus and indexers briefly hold freshly written files open, so retry a sharing
    // violation a few times before giving up on the atomic path.
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (MoveFileExW(temp.wstring().c_str(), path.wstring().c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        lastError = GetLastError();
        if (lastError != ERROR_SHARING_VIOLATION && lastError != ERROR_ACCESS_DENIED &&
            lastError != ERROR_LOCK_VIOLATION) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

#else

bool WriteAndFlush(const fs::path& path, std::string_view contents, mode_t mode, std::string& error) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        error = "Failed to open for writing: " + path.string() + " (" + std::strerror(errno) + ")";
        return false;
    }

    size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t written = ::write(fd, contents.data() + offset, contents.size() - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            error = "Failed to write: " + path.string() + " (" + std::strerror(errno) + ")";
            ::close(fd);
            return false;
        }
        offset += static_cast<size_t>(written);
    }

    int synced = ::fsync(fd);
#ifdef __APPLE__
    // fsync on macOS stops at the drive's cache; F_FULLFSYNC is the one that reaches the platter.
    if (synced == 0) synced = ::fcntl(fd, F_FULLFSYNC);
#endif
    if (synced != 0) {
        error = "Failed to flush to disk: " + path.string() + " (" + std::strerror(errno) + ")";
        ::close(fd);
        return false;
    }
    ::close(fd);
    return true;
}

void SyncDirectory(const fs::path& directory) {
    const fs::path dir = directory.empty() ? fs::path(".") : directory;
    int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}

#endif

} // namespace

bool WriteFileDurably(const fs::path& path,
                      std::string_view contents,
                      std::string& error,
                      DurableFileMode mode) {
    const fs::path temp = TempPathFor(path);

#ifdef _WIN32
    if (mode == DurableFileMode::Text && contents.find('\n') != std::string_view::npos) {
        return WriteFileDurably(path, ToWindowsText(contents), error, DurableFileMode::Binary);
    }

    if (!WriteAndFlush(temp, contents, error)) {
        std::error_code ec;
        fs::remove(temp, ec);
        return false;
    }

    DWORD renameError = 0;
    if (ReplaceWithTemp(temp, path, renameError)) {
        return true;
    }

    // Something is holding the target open. Failing the save outright loses the user's work,
    // so fall back to writing in place - not atomic, but still flushed before we report success.
    std::error_code ec;
    fs::remove(temp, ec);
    if (!WriteAndFlush(path, contents, error)) {
        error += " (atomic replace also failed: " +
                 std::system_category().message(static_cast<int>(renameError)) + ")";
        return false;
    }
    return true;
#else
    (void)mode;
    mode_t permissions = 0644;
    struct stat existing {};
    if (::stat(path.c_str(), &existing) == 0) {
        permissions = existing.st_mode & 07777;
    }

    if (!WriteAndFlush(temp, contents, permissions, error)) {
        std::error_code ec;
        fs::remove(temp, ec);
        return false;
    }
    if (::rename(temp.c_str(), path.c_str()) != 0) {
        error = "Failed to move saved file into place: " + path.string() + " (" +
                std::strerror(errno) + ")";
        std::error_code ec;
        fs::remove(temp, ec);
        return false;
    }
    SyncDirectory(path.parent_path());
    return true;
#endif
}

namespace {

// Every request for a path takes the next generation, and a write older than the one already on
// disk is dropped. Without that, a burst write the worker picked up just before a synchronous
// save could land after it and put the stale contents back.
class CoalescedWriter {
public:
    ~CoalescedWriter() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    void write(const fs::path& path, std::string contents) {
        using Clock = std::chrono::steady_clock;
        constexpr auto kBurstWindow = std::chrono::seconds(1);

        std::unique_lock<std::mutex> lock(mutex_);
        PathState& state = paths_[path];
        const Clock::time_point now = Clock::now();
        const bool inBurst = state.requested > 0 && now - state.lastRequest < kBurstWindow;
        state.lastRequest = now;
        const uint64_t generation = ++state.requested;

        if (inBurst) {
            if (!state.hasPending) ++pendingCount_;
            state.hasPending = true;
            state.pending = std::move(contents);
            state.pendingGeneration = generation;
            if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
            lock.unlock();
            wake_.notify_one();
            return;
        }

        // This save is newer than anything still queued for the path.
        if (state.hasPending) {
            state.hasPending = false;
            state.pending.clear();
            --pendingCount_;
        }
        ++inFlight_;
        lock.unlock();

        commit(path, contents, generation);

        lock.lock();
        --inFlight_;
        if (pendingCount_ == 0 && inFlight_ == 0) idle_.notify_all();
    }

    void flush() {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_.wait(lock, [this] { return pendingCount_ == 0 && inFlight_ == 0; });
    }

private:
    struct PathState {
        std::chrono::steady_clock::time_point lastRequest;
        uint64_t requested = 0;
        bool hasPending = false;
        std::string pending;
        uint64_t pendingGeneration = 0;
    };

    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return stopping_ || pendingCount_ > 0; });
            if (pendingCount_ == 0) return;

            auto it = std::find_if(paths_.begin(), paths_.end(),
                                   [](const auto& entry) { return entry.second.hasPending; });
            const fs::path path = it->first;
            const std::string contents = std::move(it->second.pending);
            const uint64_t generation = it->second.pendingGeneration;
            it->second.hasPending = false;
            it->second.pending.clear();
            --pendingCount_;
            ++inFlight_;

            lock.unlock();
            commit(path, contents, generation);
            lock.lock();

            --inFlight_;
            if (pendingCount_ == 0 && inFlight_ == 0) idle_.notify_all();
        }
    }

    void commit(const fs::path& path, const std::string& contents, uint64_t generation) {
        std::lock_guard<std::mutex> io(ioMutex_);
        uint64_t& written = writtenGeneration_[path];
        if (generation < written) return;
        std::string error;
        if (!WriteFileDurably(path, contents, error)) {
            std::cerr << "Failed to save " << path.string() << ": " << error << std::endl;
            return;
        }
        written = generation;
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::map<fs::path, PathState> paths_;
    int pendingCount_ = 0;
    int inFlight_ = 0;
    bool stopping_ = false;
    std::thread worker_;

    std::mutex ioMutex_;
    std::map<fs::path, uint64_t> writtenGeneration_;
};

CoalescedWriter& Writer() {
    static CoalescedWriter writer;
    return writer;
}

} // namespace

void WriteFileDurablyCoalesced(const fs::path& path, std::string contents, DurableFileMode mode) {
#ifdef _WIN32
    if (mode == DurableFileMode::Text) contents = ToWindowsText(contents);
#else
    (void)mode;
#endif
    Writer().write(path, std::move(contents));
}

void FlushDurableWrites() {
    Writer().flush();
}

bool FileContainsNulBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;

    std::vector<char> buffer(64 * 1024);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = in.gcount();
        if (count > 0 && std::memchr(buffer.data(), '\0', static_cast<size_t>(count)) != nullptr) {
            return true;
        }
    }
    return false;
}

} // namespace Modularity
