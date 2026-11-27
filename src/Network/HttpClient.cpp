#include "Network/HttpClient.h"

#include "Sha256.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <winhttp.h>
    #define MODU_HTTP_WINHTTP 1
#elif defined(__has_include)
    #if __has_include(<curl/curl.h>) && __has_include(<dlfcn.h>)
        #include <curl/curl.h>
        #include <dlfcn.h>
        #define MODU_HTTP_CURL 1
    #endif
#endif

namespace Net {
namespace Http {
namespace {

constexpr int kWorkerCount = 4;
constexpr const char* kUserAgent = "Modularity-Http/1.0";

// ---------------------------------------------------------------------------
// Transport backends. Each runs on a worker thread and must honour `abort`.
// ---------------------------------------------------------------------------

#if defined(MODU_HTTP_CURL)
struct CurlApi {
    void* library = nullptr;
    std::string error;
    CURLcode (*global_init)(long) = nullptr;
    CURL* (*easy_init)() = nullptr;
    CURLcode (*easy_setopt)(CURL*, CURLoption, ...) = nullptr;
    CURLcode (*easy_perform)(CURL*) = nullptr;
    CURLcode (*easy_getinfo)(CURL*, CURLINFO, ...) = nullptr;
    void (*easy_cleanup)(CURL*) = nullptr;
    curl_slist* (*slist_append)(curl_slist*, const char*) = nullptr;
    void (*slist_free_all)(curl_slist*) = nullptr;
    const char* (*easy_strerror)(CURLcode) = nullptr;

    bool Load() {
        static const char* const kNames[] = {"libcurl.so.4", "libcurl.so", "libcurl-gnutls.so.4", "libcurl.4.dylib"};
        for (const char* name : kNames) {
            library = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (library) break;
        }
        if (!library) {
            error = "libcurl was not found on this system (install curl to enable HTTP).";
            return false;
        }
#define MODU_CURL_SYM(field, symbol) \
        field = reinterpret_cast<decltype(field)>(dlsym(library, symbol)); \
        if (!field) { error = std::string("libcurl is missing ") + symbol; library = nullptr; return false; }
        MODU_CURL_SYM(global_init, "curl_global_init")
        MODU_CURL_SYM(easy_init, "curl_easy_init")
        MODU_CURL_SYM(easy_setopt, "curl_easy_setopt")
        MODU_CURL_SYM(easy_perform, "curl_easy_perform")
        MODU_CURL_SYM(easy_getinfo, "curl_easy_getinfo")
        MODU_CURL_SYM(easy_cleanup, "curl_easy_cleanup")
        MODU_CURL_SYM(slist_append, "curl_slist_append")
        MODU_CURL_SYM(slist_free_all, "curl_slist_free_all")
        MODU_CURL_SYM(easy_strerror, "curl_easy_strerror")
#undef MODU_CURL_SYM
        // Not thread-safe on older libcurl: done once, before any worker uses it.
        if (global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            error = "curl_global_init failed";
            library = nullptr;
            return false;
        }
        return true;
    }
};

CurlApi& Curl() {
    static CurlApi api;
    static std::once_flag once;
    std::call_once(once, [] { api.Load(); });
    return api;
}

size_t CurlWrite(char* data, size_t size, size_t count, void* user) {
    static_cast<std::string*>(user)->append(data, size * count);
    return size * count;
}

int CurlProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<const std::atomic<bool>*>(user)->load() ? 1 : 0;
}

Response Perform(const Request& request, const std::atomic<bool>& abort) {
    Response out;
    CurlApi& api = Curl();
    if (!api.library) {
        out.error = api.error;
        return out;
    }
    CURL* curl = api.easy_init();
    if (!curl) {
        out.error = "curl_easy_init failed";
        return out;
    }
    curl_slist* headerList = nullptr;
    for (const auto& header : request.headers) {
        headerList = api.slist_append(headerList, (header.first + ": " + header.second).c_str());
    }
    const long timeout = std::max(1, request.timeoutSeconds);
    std::string method = request.method.empty() ? std::string("GET") : request.method;
    std::transform(method.begin(), method.end(), method.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    api.easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    api.easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  // required when used from several threads
    api.easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    api.easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    api.easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    api.easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, std::min(timeout, 15L));
    api.easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    api.easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    if (headerList) api.easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    if (method == "HEAD") {
        api.easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else if (method != "GET" || !request.body.empty()) {
        api.easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
        api.easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
        if (method != "POST") api.easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    }
    api.easy_setopt(curl, CURLOPT_WRITEFUNCTION, &CurlWrite);
    api.easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
    api.easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    api.easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &CurlProgress);
    api.easy_setopt(curl, CURLOPT_XFERINFODATA, &abort);

    const CURLcode code = api.easy_perform(curl);
    if (code == CURLE_OK) {
        long status = 0;
        api.easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        out.status = static_cast<int>(status);
    } else {
        out.error = abort.load() ? std::string("Request aborted") : std::string(api.easy_strerror(code));
        out.body.clear();
    }
    if (headerList) api.slist_free_all(headerList);
    api.easy_cleanup(curl);
    return out;
}

bool BackendAvailable(std::string* reason) {
    CurlApi& api = Curl();
    if (!api.library && reason) *reason = api.error;
    return api.library != nullptr;
}

#elif defined(MODU_HTTP_WINHTTP)

std::wstring Widen(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &wide[0], count);
    return wide;
}

std::string WinHttpError(const char* step) {
    std::ostringstream message;
    message << "WinHTTP " << step << " failed (error " << GetLastError() << ")";
    return message.str();
}

Response Perform(const Request& request, const std::atomic<bool>& abort) {
    Response out;
    const std::wstring url = Widen(request.url);
    URL_COMPONENTS parts;
    ZeroMemory(&parts, sizeof(parts));
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) {
        out.error = "Invalid URL: " + request.url;
        return out;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : std::wstring());
    if (parts.lpszExtraInfo) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    const std::wstring method = Widen(request.method.empty() ? std::string("GET") : request.method);
    std::wstring headers;
    for (const auto& header : request.headers) headers += Widen(header.first + ": " + header.second + "\r\n");

    HINTERNET session = WinHttpOpen(L"Modularity-Http/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connection = nullptr;
    HINTERNET handle = nullptr;
    const int timeoutMs = std::max(1, request.timeoutSeconds) * 1000;
    do {
        if (!session) { out.error = WinHttpError("open"); break; }
        WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
        connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
        if (!connection) { out.error = WinHttpError("connect"); break; }
        handle = WinHttpOpenRequest(connection, method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!handle) { out.error = WinHttpError("request"); break; }
        void* body = request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(request.body.data());
        const DWORD bodySize = static_cast<DWORD>(request.body.size());
        if (!WinHttpSendRequest(handle, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                headers.empty() ? 0 : static_cast<DWORD>(-1), body, bodySize, bodySize, 0) ||
            !WinHttpReceiveResponse(handle, nullptr)) {
            out.error = WinHttpError("send");
            break;
        }
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
        for (;;) {
            if (abort.load()) { out.error = "Request aborted"; break; }
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(handle, &available)) { out.error = WinHttpError("read"); break; }
            if (available == 0) break;
            const size_t offset = out.body.size();
            out.body.resize(offset + available);
            DWORD read = 0;
            if (!WinHttpReadData(handle, &out.body[offset], available, &read)) { out.error = WinHttpError("read"); break; }
            out.body.resize(offset + read);
        }
        if (out.error.empty()) out.status = static_cast<int>(status);
        else out.body.clear();
    } while (false);
    if (handle) WinHttpCloseHandle(handle);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    return out;
}

bool BackendAvailable(std::string*) { return true; }

#else

Response Perform(const Request&, const std::atomic<bool>&) {
    Response out;
    out.error = "HTTP is not available on this platform build.";
    return out;
}

bool BackendAvailable(std::string* reason) {
    if (reason) *reason = "HTTP is not available on this platform build.";
    return false;
}

#endif

// ---------------------------------------------------------------------------
// Worker pool + main-thread completion queue.
// ---------------------------------------------------------------------------

struct Job {
    RequestId id = 0;
    Request request;
};

struct Finished {
    RequestId id = 0;
    Response response;
};

struct Callbacks {
    Completion complete;
    Release release;
};

struct State {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    std::deque<Finished> finished;
    std::unordered_map<RequestId, Callbacks> callbacks;
    std::vector<std::thread> workers;
    std::atomic<bool> stopping{false};
    RequestId nextId = 1;

    void WorkerLoop() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return stopping.load() || !jobs.empty(); });
                if (stopping.load()) return;
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            Finished done;
            done.id = job.id;
            done.response = Perform(job.request, stopping);
            std::lock_guard<std::mutex> lock(mutex);
            finished.push_back(std::move(done));
        }
    }
};

// Leaked on purpose: a static with joinable threads would std::terminate at exit
// when Shutdown() was never reached (crash paths, early exits).
State& GetState() {
    static State* state = new State();
    return *state;
}

void RunRelease(Callbacks& callbacks) {
    if (callbacks.release) callbacks.release();
}

} // namespace

RequestId Send(Request request, Completion onComplete, Release onRelease) {
    State& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    RequestId id = state.nextId++;
    if (id == 0) id = state.nextId++;
    if (onComplete || onRelease) state.callbacks[id] = Callbacks{std::move(onComplete), std::move(onRelease)};
    if (state.stopping.load()) {
        Finished done;
        done.id = id;
        done.response.error = "HTTP client is shut down";
        state.finished.push_back(std::move(done));
        return id;
    }
    if (state.workers.empty()) {
        for (int i = 0; i < kWorkerCount; ++i) state.workers.emplace_back([&state] { state.WorkerLoop(); });
    }
    state.jobs.push_back(Job{id, std::move(request)});
    state.wake.notify_one();
    return id;
}

void Pump(int maxCompletions) {
    State& state = GetState();
    for (int i = 0; i < maxCompletions; ++i) {
        Finished done;
        Callbacks callbacks;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.finished.empty()) return;
            done = std::move(state.finished.front());
            state.finished.pop_front();
            auto it = state.callbacks.find(done.id);
            if (it != state.callbacks.end()) {
                callbacks = std::move(it->second);
                state.callbacks.erase(it);
                found = true;
            }
        }
        if (!found) continue;  // cancelled, or fire-and-forget
        // Callbacks run without the lock so they may Send() follow-up requests.
        if (callbacks.complete) callbacks.complete(done.response);
        RunRelease(callbacks);
    }
}

void CancelCallbacks() {
    State& state = GetState();
    std::unordered_map<RequestId, Callbacks> dropped;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        dropped.swap(state.callbacks);
    }
    for (auto& entry : dropped) RunRelease(entry.second);
}

void Shutdown() {
    State& state = GetState();
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.stopping.store(true);
        state.jobs.clear();
        workers.swap(state.workers);
    }
    state.wake.notify_all();
    for (std::thread& worker : workers) {
        if (worker.joinable()) worker.join();
    }
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.finished.clear();
    }
    CancelCallbacks();
}

bool IsAvailable(std::string* reason) {
    return BackendAvailable(reason);
}

namespace {

std::string ReadFirstLine(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string line;
    if (file) std::getline(file, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

std::string MachineIdentity() {
#if defined(_WIN32)
    char guid[128] = {};
    DWORD size = sizeof(guid);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid",
                     RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, guid, &size) == ERROR_SUCCESS) {
        return std::string(guid);
    }
    return std::string();
#else
    for (const char* path : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            const std::string id = ReadFirstLine(path);
            if (!id.empty()) return id;
        }
    }
    return std::string();
#endif
}

std::filesystem::path FallbackIdPath() {
#if defined(_WIN32)
    const char* base = std::getenv("APPDATA");
    if (!base || !*base) return std::filesystem::path();
    return std::filesystem::path(base) / "Modularity" / "device-id";
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) return std::filesystem::path(xdg) / "Modularity" / "device-id";
    const char* home = std::getenv("HOME");
    if (!home || !*home) return std::filesystem::path();
    return std::filesystem::path(home) / ".local" / "share" / "Modularity" / "device-id";
#endif
}

std::string RandomHex(size_t bytes) {
    static const char* kHex = "0123456789abcdef";
    std::random_device device;
    std::string out;
    out.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        const unsigned value = device() & 0xffu;
        out.push_back(kHex[value >> 4]);
        out.push_back(kHex[value & 0xf]);
    }
    return out;
}

} // namespace

std::string DeviceUniqueId() {
    static std::mutex mutex;
    static std::string cached;
    std::lock_guard<std::mutex> lock(mutex);
    if (!cached.empty()) return cached;

    std::string identity = MachineIdentity();
    if (identity.empty()) {
        const std::filesystem::path path = FallbackIdPath();
        std::error_code ec;
        if (!path.empty()) identity = ReadFirstLine(path);
        if (identity.empty()) {
            identity = RandomHex(16);
            if (!path.empty()) {
                std::filesystem::create_directories(path.parent_path(), ec);
                std::ofstream file(path, std::ios::trunc);
                if (file) file << identity << "\n";
            }
        }
    }
    const std::string salted = "modularity-device:" + identity;
    uint8_t digest[32];
    Modularity::Sha256::hash(salted.data(), salted.size(), digest);
    cached = Modularity::Sha256::toHex(digest).substr(0, 40);
    return cached;
}

} // namespace Http
} // namespace Net
