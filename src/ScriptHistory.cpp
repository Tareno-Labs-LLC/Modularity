#include "ScriptHistory.h"

#include "PackageManager.h"
#include "ScriptRuntime.h"

namespace {
    constexpr const char* kHistoryFileVersion = "1";

    std::string trimCopy(const std::string& value) {
        size_t begin = value.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) return std::string();
        size_t end = value.find_last_not_of(" \t\r\n");
        return value.substr(begin, end - begin + 1);
    }

    bool splitKeyValue(const std::string& line, std::string& outKey, std::string& outValue) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) return false;
        outKey = trimCopy(line.substr(0, eq));
        outValue = line.substr(eq + 1);
        if (!outValue.empty() && outValue.back() == '\r') outValue.pop_back();
        return !outKey.empty();
    }
}

std::string ScriptHistory::makeKey(const fs::path& path) {
    if (path.empty()) return std::string();
    std::error_code ec;
    fs::path absolutePath = fs::absolute(path, ec);
    if (ec) absolutePath = path;
    return absolutePath.lexically_normal().string();
}

long long ScriptHistory::toTimeRep(const fs::file_time_type& time) {
    return static_cast<long long>(time.time_since_epoch().count());
}

fs::path ScriptHistory::fileForOutDir(const fs::path& outDir) {
    if (outDir.empty()) return fs::path();
    return outDir / "ScriptHistory.modu";
}

bool scriptToolchainStampsCompatible(const ScriptToolchainStamp& built,
                                     const ScriptToolchainStamp& current) {
    if (!built.valid() || !current.valid()) return false;
    if (built.abiVersion != current.abiVersion) return false;
    if (built.layoutSignature != 0 && current.layoutSignature != 0 &&
        built.layoutSignature != current.layoutSignature) {
        return false;
    }
    if (!built.engineVersion.empty() && !current.engineVersion.empty() &&
        built.engineVersion != current.engineVersion) {
        return false;
    }
    if (!built.apiStamp.empty() && !current.apiStamp.empty() &&
        built.apiStamp != current.apiStamp) {
        return false;
    }
    return true;
}

ScriptToolchainStamp currentScriptToolchainStamp(const std::vector<fs::path>& includeDirs) {
    ScriptToolchainStamp stamp;
    stamp.abiVersion = MODULARITY_NATIVE_SCRIPT_ABI_VERSION;
    stamp.layoutSignature = MODULARITY_SCRIPT_LAYOUT_SIGNATURE();
    stamp.engineVersion = PackageManager::currentEngineVersion();

    for (const fs::path& dir : includeDirs) {
        if (dir.empty()) continue;
        const fs::path header = dir / "ScriptRuntime.h";
        std::error_code ec;
        if (!fs::exists(header, ec) || ec) continue;
        const uintmax_t size = fs::file_size(header, ec);
        if (ec) continue;
        const fs::file_time_type writeTime = fs::last_write_time(header, ec);
        if (ec) continue;
        stamp.apiStamp = std::to_string(size) + ":" +
                         std::to_string(ScriptHistory::toTimeRep(writeTime));
        break;
    }

    return stamp;
}

void ScriptHistory::reset() {
    stamp_ = ScriptToolchainStamp();
    entries_.clear();
    migrating_ = true;
    dirty_ = false;
}

void ScriptHistory::setStamp(const ScriptToolchainStamp& stamp) {
    if (stamp_.abiVersion == stamp.abiVersion &&
        stamp_.layoutSignature == stamp.layoutSignature &&
        stamp_.engineVersion == stamp.engineVersion &&
        stamp_.apiStamp == stamp.apiStamp) {
        return;
    }
    stamp_ = stamp;
    dirty_ = true;
}

const ScriptHistoryEntry* ScriptHistory::find(const std::string& sourceKey) const {
    auto it = entries_.find(sourceKey);
    return it == entries_.end() ? nullptr : &it->second;
}

void ScriptHistory::record(const std::string& sourceKey, const ScriptHistoryEntry& entry) {
    if (sourceKey.empty()) return;
    entries_[sourceKey] = entry;
    dirty_ = true;
}

void ScriptHistory::forget(const std::string& sourceKey) {
    if (entries_.erase(sourceKey) > 0) {
        dirty_ = true;
    }
}

void ScriptHistory::clearEntries() {
    if (entries_.empty()) return;
    entries_.clear();
    dirty_ = true;
}

void ScriptHistory::retainOnly(const std::unordered_set<std::string>& sourceKeys) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (sourceKeys.find(it->first) == sourceKeys.end()) {
            it = entries_.erase(it);
            dirty_ = true;
        } else {
            ++it;
        }
    }
}

bool ScriptHistory::load(const fs::path& file) {
    reset();
    if (file.empty()) return false;

    std::ifstream in(file);
    if (!in.is_open()) return false;

    std::string line;
    bool inEntry = false;
    std::string pendingKey;
    ScriptHistoryEntry pending;
    auto flushEntry = [&]() {
        if (inEntry && !pendingKey.empty()) {
            entries_[pendingKey] = pending;
        }
        inEntry = false;
        pendingKey.clear();
        pending = ScriptHistoryEntry();
    };

    while (std::getline(in, line)) {
        const std::string trimmed = trimCopy(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        if (trimmed == "[script]") {
            flushEntry();
            inEntry = true;
            continue;
        }

        std::string key;
        std::string value;
        if (!splitKeyValue(line, key, value)) continue;

        ScriptToolchainStamp& target = inEntry ? pending.stamp : stamp_;
        if (key == "version") {
            if (!inEntry && trimCopy(value) != kHistoryFileVersion) {
                reset();
                return false;
            }
        } else if (key == "abi") {
            target.abiVersion = std::atoi(trimCopy(value).c_str());
        } else if (key == "layout") {
            target.layoutSignature = std::strtoull(trimCopy(value).c_str(), nullptr, 10);
        } else if (key == "engine") {
            target.engineVersion = trimCopy(value);
        } else if (key == "apiStamp") {
            target.apiStamp = trimCopy(value);
        } else if (inEntry && key == "source") {
            pendingKey = value;
        } else if (inEntry && key == "binary") {
            pending.binaryPath = fs::path(value);
        } else if (inEntry && key == "binaryTime") {
            pending.binaryWriteTime = std::strtoll(trimCopy(value).c_str(), nullptr, 10);
        }
    }
    flushEntry();

    migrating_ = false;
    dirty_ = false;
    return true;
}

bool ScriptHistory::save(const fs::path& file, std::string& error) {
    if (file.empty()) {
        error = "No script history path";
        return false;
    }

    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);

    std::ofstream out(file, std::ios::trunc);
    if (!out.is_open()) {
        error = "Failed to write script history: " + file.string();
        return false;
    }

    out << "# Modularity compiled-script history. Generated - safe to delete.\n";
    out << "# Records which engine build produced each compiled script so an ABI or\n";
    out << "# layout change can force a rebuild instead of failing at load time.\n";
    out << "version=" << kHistoryFileVersion << "\n";
    out << "abi=" << stamp_.abiVersion << "\n";
    out << "layout=" << stamp_.layoutSignature << "\n";
    out << "engine=" << stamp_.engineVersion << "\n";
    out << "apiStamp=" << stamp_.apiStamp << "\n";

    for (const auto& entry : entries_) {
        out << "\n[script]\n";
        out << "source=" << entry.first << "\n";
        out << "binary=" << entry.second.binaryPath.string() << "\n";
        out << "abi=" << entry.second.stamp.abiVersion << "\n";
        out << "layout=" << entry.second.stamp.layoutSignature << "\n";
        out << "engine=" << entry.second.stamp.engineVersion << "\n";
        out << "apiStamp=" << entry.second.stamp.apiStamp << "\n";
        out << "binaryTime=" << entry.second.binaryWriteTime << "\n";
    }

    out.flush();
    if (!out) {
        error = "Failed to write script history: " + file.string();
        return false;
    }

    migrating_ = false;
    dirty_ = false;
    return true;
}
