#pragma once

// Asynchronous HTTP(S) client shared by the editor and the standalone player.
//
// Requests run on a small worker pool; completions are queued and delivered on the
// main thread from Pump(), so a completion may touch the scene, the scripting
// runtime or the UI without locking. Nothing here blocks the render loop.
//
// Backends:
//   - Linux / other POSIX: libcurl, resolved with dlopen at first use, so neither
//     the editor nor the player gains a hard link dependency. When libcurl is
//     missing every request completes with status 0 and an explanatory error.
//   - Windows: WinHTTP (system component, always present).
//   - Anything else: requests complete immediately with an "unavailable" error.

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Net {
namespace Http {

struct Request {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int timeoutSeconds = 30;
};

struct Response {
    // HTTP status, or 0 when no response arrived (DNS, TLS, timeout, offline...).
    int status = 0;
    std::string body;
    // Transport error text; empty when a response (of any status) arrived.
    std::string error;
};

using RequestId = uint32_t;
using Completion = std::function<void(const Response&)>;
// Called exactly once after the completion ran, or instead of it when the request's
// callback was cancelled. Lets callers free whatever the completion captured.
using Release = std::function<void()>;

// Queue a request. Returns a non-zero id. The completion (and then the release)
// always runs on the thread that calls Pump(), never inside Send().
RequestId Send(Request request, Completion onComplete, Release onRelease = Release());

// Deliver finished requests. Call once per frame from the main thread.
void Pump(int maxCompletions = 64);

// Drop every pending completion without running it (the release still runs).
// The network requests themselves keep going; only their callbacks are forgotten.
// Used when play mode stops so callbacks do not reach destroyed script instances.
void CancelCallbacks();

// Stop the worker threads and abort in-flight transfers. Pending callbacks are released.
void Shutdown();

// True when a transport backend is available. On false, `reason` says why.
bool IsAvailable(std::string* reason = nullptr);

// Stable, anonymous identifier for this machine (SHA-256 of the OS machine id,
// salted so it cannot be matched against other software). Falls back to a random
// id persisted in the user's data directory.
std::string DeviceUniqueId();

} // namespace Http
} // namespace Net
