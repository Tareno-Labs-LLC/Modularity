#pragma once

// ModuHttp - HTTP(S) requests, JSON, and cross-script shared values for scripts.
//
//     add ModuCPP;
//     add ModuHttp;
//
//     void Begin() {
//         Http.Get("https://example.com/status.json", [this](const HttpResponse& r) {
//             if (!r.Ok()) { Console.Log("failed: " + r.Error); return; }
//             Json status = r.ParseJson();
//             Console.Log("players online: " + status["Players"].AsString());
//         });
//     }
//
// Requests never block: the callback runs on the main thread on a later frame, with
// the script's context restored, so it can touch the scene like any update method.
// When play mode stops, pending callbacks are dropped (never called) so they cannot
// reach a destroyed script instance.
//
// Engine side: ScriptContext::HttpSend (src/Network/HttpClient.cpp). libcurl on
// Linux (loaded on demand), WinHTTP on Windows.

#include "ModuCPPScriptApi.h"

#include <cctype>
#include <cmath>
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ModuCPP {

// ---------------------------------------------------------------------------
// Json - a small dynamic JSON value. Objects keep insertion order.
// ---------------------------------------------------------------------------
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : type_(Type::Bool), bool_(value) {}
    Json(int value) : type_(Type::Number), number_(value) {}
    Json(unsigned value) : type_(Type::Number), number_(value) {}
    Json(unsigned long value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(unsigned long long value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(long value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(long long value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(float value) : type_(Type::Number), number_(value) {}
    Json(double value) : type_(Type::Number), number_(value) {}
    Json(const char* value) : type_(value ? Type::String : Type::Null), string_(value ? value : "") {}
    Json(std::string value) : type_(Type::String), string_(std::move(value)) {}
    Json(const std::vector<std::string>& values) : type_(Type::Array) {
        for (const auto& v : values) items_.emplace_back(v);
    }

    static Json Object() { Json j; j.type_ = Type::Object; return j; }
    static Json Array() { Json j; j.type_ = Type::Array; return j; }
    // Json::Map({{"Name", "Ada"}, {"Score", 12}})
    static Json Map(std::initializer_list<std::pair<std::string, Json>> members) {
        Json j = Object();
        for (const auto& m : members) j.Set(m.first, m.second);
        return j;
    }
    // Json::List({1, "two", true})
    static Json List(std::initializer_list<Json> values) {
        Json j = Array();
        for (const auto& v : values) j.items_.push_back(v);
        return j;
    }

    Type GetType() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsBool() const { return type_ == Type::Bool; }
    bool IsNumber() const { return type_ == Type::Number; }
    bool IsString() const { return type_ == Type::String; }
    bool IsArray() const { return type_ == Type::Array; }
    bool IsObject() const { return type_ == Type::Object; }

    bool AsBool(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    double AsNumber(double fallback = 0.0) const { return type_ == Type::Number ? number_ : fallback; }
    int AsInt(int fallback = 0) const { return type_ == Type::Number ? static_cast<int>(number_) : fallback; }
    float AsFloat(float fallback = 0.0f) const { return type_ == Type::Number ? static_cast<float>(number_) : fallback; }
    // Strings as-is; numbers and bools converted (bools as "True"/"False", which is
    // what services written against .NET's Convert.ToString compare against).
    std::string AsString(const std::string& fallback = std::string()) const {
        switch (type_) {
            case Type::String: return string_;
            case Type::Number: return FormatNumber(number_);
            case Type::Bool: return bool_ ? "True" : "False";
            case Type::Null: return fallback;
            default: return Dump();
        }
    }

    // Arrays and objects.
    size_t Size() const { return type_ == Type::Array ? items_.size() : (type_ == Type::Object ? keys_.size() : 0); }
    bool Empty() const { return Size() == 0; }

    // Array access. Out of range or wrong type returns a shared null.
    const Json& operator[](size_t index) const {
        return (type_ == Type::Array && index < items_.size()) ? items_[index] : NullRef();
    }
    const Json& operator[](int index) const { return index < 0 ? NullRef() : (*this)[static_cast<size_t>(index)]; }
    void Push(Json value) {
        if (type_ != Type::Array) { *this = Array(); }
        items_.push_back(std::move(value));
    }
    const std::vector<Json>& Items() const { return items_; }

    // Object access. Missing keys on a const value return a shared null; the
    // non-const operator[] inserts (turning a null into an object), like std::map.
    const Json& operator[](const std::string& key) const {
        const int i = IndexOf(key);
        return i < 0 ? NullRef() : values_[static_cast<size_t>(i)];
    }
    const Json& operator[](const char* key) const { return (*this)[std::string(key)]; }
    Json& operator[](const std::string& key) {
        if (type_ != Type::Object) *this = Object();
        const int i = IndexOf(key);
        if (i >= 0) return values_[static_cast<size_t>(i)];
        keys_.push_back(key);
        values_.emplace_back();
        return values_.back();
    }
    Json& operator[](const char* key) { return (*this)[std::string(key)]; }
    bool Has(const std::string& key) const { return IndexOf(key) >= 0; }
    void Set(const std::string& key, Json value) { (*this)[key] = std::move(value); }
    bool Remove(const std::string& key) {
        const int i = IndexOf(key);
        if (i < 0) return false;
        keys_.erase(keys_.begin() + i);
        values_.erase(values_.begin() + i);
        return true;
    }
    const std::vector<std::string>& Keys() const { return keys_; }
    const std::vector<Json>& Values() const { return values_; }

    // Dotted path lookup through nested objects: Get("Kick.Reason").
    const Json& Get(const std::string& path) const {
        const Json* current = this;
        size_t start = 0;
        while (start <= path.size()) {
            const size_t dot = path.find('.', start);
            const std::string part = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            if (!current->IsObject()) return NullRef();
            current = &(*current)[part];
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        return *current;
    }

    static const Json& NullRef() {
        static const Json null;
        return null;
    }

    // ---- Serialization ----
    std::string Dump() const {
        std::string out;
        Write(out);
        return out;
    }

    // Returns null (and ok=false) on malformed input, never throws.
    static Json Parse(const std::string& text, bool* ok = nullptr) {
        size_t i = 0;
        Json value;
        bool success = !text.empty() && ReadValue(text, i, value, 0);
        if (success) {
            SkipSpace(text, i);
            success = i == text.size();
        }
        if (ok) *ok = success;
        return success ? value : Json();
    }

    static std::string FormatNumber(double value) {
        if (std::isnan(value) || std::isinf(value)) return "0";
        if (value == std::floor(value) && std::fabs(value) < 1e15) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.0f", value);
            return buffer;
        }
        char buffer[40];
        for (int precision = 15; precision <= 17; ++precision) {
            std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
            if (std::strtod(buffer, nullptr) == value) break;
        }
        return buffer;
    }

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Json> items_;
    std::vector<std::string> keys_;
    std::vector<Json> values_;

    int IndexOf(const std::string& key) const {
        if (type_ != Type::Object) return -1;
        for (size_t i = 0; i < keys_.size(); ++i) {
            if (keys_[i] == key) return static_cast<int>(i);
        }
        return -1;
    }

    static void WriteString(std::string& out, const std::string& s) {
        out.push_back('"');
        for (unsigned char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (c < 0x20) {
                        char buffer[8];
                        std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                        out += buffer;
                    } else {
                        out.push_back(static_cast<char>(c));
                    }
            }
        }
        out.push_back('"');
    }

    void Write(std::string& out) const {
        switch (type_) {
            case Type::Null: out += "null"; break;
            case Type::Bool: out += bool_ ? "true" : "false"; break;
            case Type::Number: out += FormatNumber(number_); break;
            case Type::String: WriteString(out, string_); break;
            case Type::Array:
                out.push_back('[');
                for (size_t i = 0; i < items_.size(); ++i) {
                    if (i) out.push_back(',');
                    items_[i].Write(out);
                }
                out.push_back(']');
                break;
            case Type::Object:
                out.push_back('{');
                for (size_t i = 0; i < keys_.size(); ++i) {
                    if (i) out.push_back(',');
                    WriteString(out, keys_[i]);
                    out.push_back(':');
                    values_[i].Write(out);
                }
                out.push_back('}');
                break;
        }
    }

    static void SkipSpace(const std::string& s, size_t& i) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }

    static void AppendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    static bool ReadHex4(const std::string& s, size_t& i, unsigned& out) {
        if (i + 4 > s.size()) return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    static bool ReadString(const std::string& s, size_t& i, std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (i >= s.size()) return false;
            const char e = s[i++];
            switch (e) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!ReadHex4(s, i, cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        size_t j = i + 2;
                        unsigned low = 0;
                        if (ReadHex4(s, j, low) && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            i = j;
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: out.push_back(e); break;
            }
        }
        return false;
    }

    static bool ReadValue(const std::string& s, size_t& i, Json& out, int depth) {
        if (depth > 256) return false;
        SkipSpace(s, i);
        if (i >= s.size()) return false;
        const char c = s[i];
        if (c == '{') {
            out = Object();
            ++i;
            SkipSpace(s, i);
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            for (;;) {
                SkipSpace(s, i);
                std::string key;
                if (!ReadString(s, i, key)) return false;
                SkipSpace(s, i);
                if (i >= s.size() || s[i++] != ':') return false;
                Json value;
                if (!ReadValue(s, i, value, depth + 1)) return false;
                out[key] = std::move(value);
                SkipSpace(s, i);
                if (i >= s.size()) return false;
                if (s[i] == ',') { ++i; continue; }
                if (s[i] == '}') { ++i; return true; }
                return false;
            }
        }
        if (c == '[') {
            out = Array();
            ++i;
            SkipSpace(s, i);
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            for (;;) {
                Json value;
                if (!ReadValue(s, i, value, depth + 1)) return false;
                out.items_.push_back(std::move(value));
                SkipSpace(s, i);
                if (i >= s.size()) return false;
                if (s[i] == ',') { ++i; continue; }
                if (s[i] == ']') { ++i; return true; }
                return false;
            }
        }
        if (c == '"') {
            std::string text;
            if (!ReadString(s, i, text)) return false;
            out = Json(std::move(text));
            return true;
        }
        if (s.compare(i, 4, "true") == 0) { i += 4; out = Json(true); return true; }
        if (s.compare(i, 5, "false") == 0) { i += 5; out = Json(false); return true; }
        if (s.compare(i, 4, "null") == 0) { i += 4; out = Json(); return true; }
        const size_t start = i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '+' || s[i] == '-' ||
                                s[i] == '.' || s[i] == 'e' || s[i] == 'E')) ++i;
        if (i == start) return false;
        const std::string number = s.substr(start, i - start);
        char* end = nullptr;
        const double value = std::strtod(number.c_str(), &end);
        if (!end || *end != '\0') return false;
        out = Json(value);
        return true;
    }
};

// ---------------------------------------------------------------------------
// Http
// ---------------------------------------------------------------------------
struct HttpResponse {
    int Status = 0;          // HTTP status; 0 when no response arrived
    std::string Body;
    std::string Error;       // transport error (DNS, TLS, timeout, offline); empty otherwise

    bool Ok() const { return Error.empty() && Status >= 200 && Status < 300; }
    Json ParseJson(bool* ok = nullptr) const { return Json::Parse(Body, ok); }
};

using HttpCallback = std::function<void(const HttpResponse&)>;
using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

namespace detail {
struct HttpCallbackBox {
    HttpCallback callback;
};

inline void HttpComplete(void* user, ScriptContext& ctx, int status, const char* body, size_t bodyLength,
                         const char* error) {
    auto* box = static_cast<HttpCallbackBox*>(user);
    if (!box || !box->callback) return;
    Scope scope(ctx);
    HttpResponse response;
    response.Status = status;
    if (body && bodyLength) response.Body.assign(body, bodyLength);
    response.Error = error ? error : "";
    try {
        box->callback(response);
    } catch (const std::exception& e) {
        ctx.AddConsoleMessage(std::string("[Http] callback threw: ") + e.what(), ConsoleMessageType::Error);
    } catch (...) {
        ctx.AddConsoleMessage("[Http] callback threw an unknown exception", ConsoleMessageType::Error);
    }
}

inline void HttpRelease(void* user) {
    delete static_cast<HttpCallbackBox*>(user);
}
} // namespace detail

struct HttpFacade {
    // Generic request. Returns a request id (0 when there is no script context).
    unsigned int Send(const std::string& method, const std::string& url, const std::string& body,
                      HttpCallback callback, const HttpHeaders& headers = HttpHeaders(),
                      int timeoutSeconds = 30) const {
        ScriptContext* c = ctxPtr();
        if (!c) return 0;
        std::string headerLines;
        for (const auto& h : headers) {
            headerLines += h.first;
            headerLines += ": ";
            headerLines += h.second;
            headerLines += '\n';
        }
        auto* box = callback ? new detail::HttpCallbackBox{std::move(callback)} : nullptr;
        return c->HttpSend(method, url, headerLines, body, timeoutSeconds,
                           box ? &detail::HttpComplete : nullptr,
                           box ? &detail::HttpRelease : nullptr, box);
    }

    unsigned int Get(const std::string& url, HttpCallback callback,
                     const HttpHeaders& headers = HttpHeaders(), int timeoutSeconds = 30) const {
        return Send("GET", url, std::string(), std::move(callback), headers, timeoutSeconds);
    }

    unsigned int Post(const std::string& url, const std::string& body, HttpCallback callback,
                      const HttpHeaders& headers = HttpHeaders(), int timeoutSeconds = 30) const {
        return Send("POST", url, body, std::move(callback), headers, timeoutSeconds);
    }

    // POST with Content-Type: application/json.
    unsigned int PostJson(const std::string& url, const Json& body, HttpCallback callback,
                          HttpHeaders headers = HttpHeaders(), int timeoutSeconds = 30) const {
        headers.emplace_back("Content-Type", "application/json");
        return Send("POST", url, body.Dump(), std::move(callback), headers, timeoutSeconds);
    }

    bool Available() const {
        ScriptContext* c = ctxPtr();
        std::string reason;
        return c && c->IsHttpAvailable(reason);
    }

    std::string UnavailableReason() const {
        ScriptContext* c = ctxPtr();
        if (!c) return "No script context";
        std::string reason;
        return c->IsHttpAvailable(reason) ? std::string() : reason;
    }

    // Anonymous stable machine id, like Unity's SystemInfo.deviceUniqueIdentifier.
    std::string DeviceId() const {
        ScriptContext* c = ctxPtr();
        return c ? c->GetDeviceUniqueId() : std::string();
    }
};

inline const HttpFacade Http{};

// ---------------------------------------------------------------------------
// SharedValues - strings visible to every script (each script is its own module,
// so a plain static is per-script). Cleared when play mode starts and stops.
// ---------------------------------------------------------------------------
struct SharedValuesFacade {
    void Set(const std::string& key, const std::string& value) const {
        if (ScriptContext* c = ctxPtr()) c->SetSharedValue(key, value);
    }
    std::string Get(const std::string& key, const std::string& fallback = std::string()) const {
        ScriptContext* c = ctxPtr();
        return c ? c->GetSharedValue(key, fallback) : fallback;
    }
    bool Has(const std::string& key) const {
        ScriptContext* c = ctxPtr();
        return c && c->HasSharedValue(key);
    }
    void Erase(const std::string& key) const {
        if (ScriptContext* c = ctxPtr()) c->EraseSharedValue(key);
    }
    // Increments on every Set of the key; poll it to notice changes made elsewhere.
    unsigned int Revision(const std::string& key) const {
        ScriptContext* c = ctxPtr();
        return c ? c->GetSharedValueRevision(key) : 0u;
    }
};

inline const SharedValuesFacade SharedValues{};

}  // namespace ModuCPP
