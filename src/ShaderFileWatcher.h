#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// Shader and failed-texture timestamp polling must never do filesystem I/O
// while submitting a draw. The GL thread still owns reloads and texture uploads.
class ShaderFileWatcher {
public:
    using Time = std::filesystem::file_time_type;
    using Clock = std::chrono::steady_clock;
    using ReadTime = Time (*)(const std::string&, std::error_code&);
    explicit ShaderFileWatcher(ReadTime read = [](const std::string& path, std::error_code& error) {
        return std::filesystem::last_write_time(path, error);
    }) : readTime(read) {}
    void seed(const std::string& path, Time time) {
        std::lock_guard<std::mutex> lock(mutex);
        Entry& entry = files[path];
        entry.time = time;
        ++entry.generation;
    }
    ~ShaderFileWatcher() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        if (worker.joinable()) worker.join();
    }

    Time poll(const std::string& path, Time initial) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!worker.joinable()) worker = std::thread([this] { run(); });
        auto [it, inserted] = files.try_emplace(path);
        Entry& entry = it->second;
        if (inserted) entry.time = initial;
        const auto now = Clock::now();
        if (!entry.pending && now >= entry.nextCheck) {
            requests.push_back(path);
            entry.pending = true;
            entry.nextCheck = now + std::chrono::milliseconds(500);
            wake.notify_one();
        }
        return entry.time;
    }

private:
    struct Entry {
        Time time{};
        Clock::time_point nextCheck{};
        bool pending = false;
        uint64_t generation = 0;
    };
    void run() {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            wake.wait(lock, [this] { return stopping || !requests.empty(); });
            if (stopping) return;
            std::string path = std::move(requests.front());
            requests.pop_front();
            const auto generation = files.at(path).generation;
            lock.unlock();
            std::error_code error;
            const Time time = readTime(path, error);
            lock.lock();
            Entry& entry = files.at(path);
            // Match hot reload's existing policy for absent/unreadable files:
            // keep the working program, retry later, never throw on the GL thread.
            if (!error && entry.generation == generation) entry.time = time;
            entry.pending = false;
        }
    }
    std::mutex mutex;
    std::condition_variable wake;
    std::unordered_map<std::string, Entry> files;
    std::deque<std::string> requests;
    bool stopping = false;
    std::thread worker;
    ReadTime readTime;
};
