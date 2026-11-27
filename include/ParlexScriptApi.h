#pragma once

// Parlex - CSVRStudios' game backend (accounts, bans, friends, economy, server gate)
// for ModuCPP. Port of the Parlex for Unity runtime SDK (com.csvrstudios.parlex 1.13.0).
//
//     add ModuCPP;
//     add Parlex;
//
//     void Begin() {
//         Parlex.Configure("https://parlex.csvrstudios.com", "<your title id>");
//         Parlex.LoginWithDevice([this](const ParlexResult& r) {
//             if (r.Ok) AddLog("[Parlex] signed in as " + Parlex.PlayerId());
//             else AddLog("[Parlex] sign-in failed: " + r.Error, Type.Warning);
//         });
//     }
//
// Differences from the Unity SDK:
//   - Properties are methods: Parlex.LoggedIn(), Parlex.PlayerId(), ParlexBan.IsBanned().
//   - Results carry Json (ModuHttpScriptApi.h) instead of Dictionary<string, object>;
//     r.GetList("Friends") yields Json values, read fields with v["DisplayName"].AsString().
//   - Session, player id, configuration, ban and server status are shared by every
//     script (they live in SharedValues). Event listeners (ParlexBan.OnBanned,
//     ParlexServerGate.OnChanged, ...) fire in the script module that received the
//     response; other scripts can watch ParlexServerGate.Revision() instead.
//   - Everything is reset when play mode starts or stops (Unity's domain reload).
//   - The Unity UI panels, Photon helpers and editor tooling are not part of this
//     header; Scripts/ParlexBootstrap.moducpp covers ParlexBootstrap/ServerGate.
//   - The Title secret is never needed at runtime, exactly as in the Unity SDK.

#include "ModuCPPScriptApi.h"
#include "ModuHttpScriptApi.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ModuCPP {

constexpr const char* ParlexVersion = "1.13.0";

// ---------------------------------------------------------------------------
// Result
// ---------------------------------------------------------------------------
struct ParlexResult {
    bool Ok = false;
    int Code = 0;
    std::string ErrorName;
    std::string Error;
    Json Data = Json::Object();
    std::string Raw;
    std::string BanReason;
    std::string BanExpires;
    std::string BanStarted;

    const Json& Get(const std::string& path) const { return Data.Get(path); }
    std::string GetString(const std::string& path, const std::string& fallback = std::string()) const {
        const Json& v = Get(path);
        return v.IsNull() ? fallback : v.AsString(fallback);
    }
    int GetInt(const std::string& path, int fallback = 0) const { return Get(path).AsInt(fallback); }
    bool GetBool(const std::string& path, bool fallback = false) const { return Get(path).AsBool(fallback); }
    // Items of the array at `path` (empty when missing or not an array).
    const std::vector<Json>& GetList(const std::string& path) const {
        static const std::vector<Json> empty;
        const Json& v = Get(path);
        return v.IsArray() ? v.Items() : empty;
    }
    // The object at `path` (an empty object when missing).
    Json GetMap(const std::string& path) const {
        const Json& v = Get(path);
        return v.IsObject() ? v : Json::Object();
    }

    static ParlexResult FromResponse(int code, const std::string& text, const std::string& netError) {
        ParlexResult res;
        res.Code = code;
        res.Raw = text;
        bool parsedOk = false;
        const Json parsed = Json::Parse(res.Raw, &parsedOk);
        const bool isObject = parsedOk && parsed.IsObject();
        if (isObject && parsed["data"].IsObject() && res.Code == 200) {
            res.Ok = true;
            res.Data = parsed["data"];
            return res;
        }
        res.Error = isObject && parsed.Has("errorMessage") ? parsed["errorMessage"].AsString()
                                                           : (netError.empty() ? "No answer from Parlex" : netError);
        res.ErrorName = isObject && parsed.Has("error") ? parsed["error"].AsString() : "NoConnection";
        if (res.ErrorName == "AccountBanned" && isObject && parsed["errorDetails"].IsObject()) {
            const Json& details = parsed["errorDetails"];
            if (!details.Keys().empty()) {
                res.BanReason = details.Keys()[0];
                const Json& times = details.Values()[0];
                res.BanExpires = times.Size() > 0 ? times[0].AsString() : "";
                res.BanStarted = times.Size() > 1 ? times[1].AsString() : "";
            }
        }
        return res;
    }

    static ParlexResult Failure(const std::string& errorName, const std::string& error) {
        ParlexResult res;
        res.ErrorName = errorName;
        res.Error = error;
        return res;
    }
};

using ParlexCallback = std::function<void(const ParlexResult&)>;

// ---------------------------------------------------------------------------
// Shared state + helpers (not part of the public surface)
// ---------------------------------------------------------------------------
namespace parlex_detail {

constexpr const char* kDefaultHost = "https://parlex.csvrstudios.com";

inline std::string Read(const char* key, const std::string& fallback = std::string()) {
    ScriptContext* c = ctxPtr();
    return c ? c->GetSharedValue(std::string("parlex.") + key, fallback) : fallback;
}
inline void Write(const char* key, const std::string& value) {
    if (ScriptContext* c = ctxPtr()) c->SetSharedValue(std::string("parlex.") + key, value);
}
inline bool Exists(const char* key) {
    ScriptContext* c = ctxPtr();
    return c && c->HasSharedValue(std::string("parlex.") + key);
}
inline unsigned int Revision(const char* key) {
    ScriptContext* c = ctxPtr();
    return c ? c->GetSharedValueRevision(std::string("parlex.") + key) : 0u;
}

inline double NowUnix() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(system_clock::now().time_since_epoch()).count();
}

// Module-local state (listeners, caches) must not outlive a play session: the shared
// store is wiped when play starts/stops, so a missing epoch means "new session".
struct ModuleState;
inline ModuleState& Module();

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm).
inline long long DaysFromCivil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

// Parses ISO-8601 ("2026-11-24T12:41:47.123Z", "+02:00" offsets, space separator,
// date only) or a bare Unix timestamp (seconds or milliseconds). Times without an
// offset are UTC, matching the Unity SDK's AssumeUniversal.
inline bool ParseTime(const std::string& text, double& outUnix) {
    if (text.empty()) return false;
    bool digitsOnly = true;
    for (char ch : text) if (!(ch >= '0' && ch <= '9') && ch != '.') { digitsOnly = false; break; }
    if (digitsOnly) {
        const double v = std::strtod(text.c_str(), nullptr);
        outUnix = v > 1e11 ? v / 1000.0 : v;
        return true;
    }
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    double second = 0.0;
    size_t i = 0;
    auto readInt = [&](int digits, int& out) {
        out = 0;
        for (int k = 0; k < digits; ++k, ++i) {
            if (i >= text.size() || text[i] < '0' || text[i] > '9') return false;
            out = out * 10 + (text[i] - '0');
        }
        return true;
    };
    auto expect = [&](char ch) { if (i < text.size() && text[i] == ch) { ++i; return true; } return false; };
    if (!readInt(4, year) || !expect('-') || !readInt(2, month) || !expect('-') || !readInt(2, day)) return false;
    double offsetSeconds = 0.0;
    if (i < text.size() && (text[i] == 'T' || text[i] == 't' || text[i] == ' ')) {
        ++i;
        if (!readInt(2, hour) || !expect(':') || !readInt(2, minute)) return false;
        if (expect(':')) {
            int whole = 0;
            if (!readInt(2, whole)) return false;
            second = whole;
            if (i < text.size() && (text[i] == '.' || text[i] == ',')) {
                ++i;
                double scale = 0.1;
                while (i < text.size() && text[i] >= '0' && text[i] <= '9') { second += (text[i] - '0') * scale; scale *= 0.1; ++i; }
            }
        }
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            const int sign = text[i] == '-' ? -1 : 1;
            ++i;
            int oh = 0, om = 0;
            if (!readInt(2, oh)) return false;
            expect(':');
            readInt(2, om);
            offsetSeconds = sign * (oh * 3600.0 + om * 60.0);
        }
    }
    if (month < 1 || month > 12 || day < 1 || day > 31) return false;
    const long long days = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    outUnix = static_cast<double>(days) * 86400.0 + hour * 3600.0 + minute * 60.0 + second - offsetSeconds;
    return true;
}

inline std::string TwoDigits(long long v) {
    std::string s = std::to_string(v);
    return s.size() < 2 ? "0" + s : s;
}

inline std::string Lower(std::string v) {
    for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return v;
}

inline std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

}  // namespace parlex_detail

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------
struct ParlexServerInfo {
    std::string Id, Name, Kind, Product, AppId, Region, Status = "online", Message, Address;
    int Players = 0, MaxPlayers = 0;
    bool InviteOnly = false, CanJoin = true, HostLeaveAll = false;
    bool IsOnline() const { return Status == "online"; }
};

struct ParlexStatus {
    bool GameOpen = true;
    std::string Message;
    std::string KickToken, KickReason, MuteReason;
    bool Muted = false;
    std::string AnnouncementId, AnnouncementText, AnnouncementFrom, MuteEnds;
    bool AnnouncementOnce = false;
    std::vector<std::string> LockedRooms, ShadowMuted;
    std::vector<ParlexServerInfo> Servers;

    const ParlexServerInfo* Find(const std::string& id) const {
        for (const auto& s : Servers) if (s.Id == id) return &s;
        return nullptr;
    }
    const ParlexServerInfo* FindKind(const std::string& kind) const {
        for (const auto& s : Servers) if (s.Kind == kind) return &s;
        return nullptr;
    }
    const ParlexServerInfo* FindApp(const std::string& appId) const {
        if (appId.empty()) return nullptr;
        for (const auto& s : Servers) {
            if (s.AppId.size() != appId.size()) continue;
            bool same = true;
            for (size_t i = 0; i < appId.size() && same; ++i) {
                same = std::tolower(static_cast<unsigned char>(s.AppId[i])) == std::tolower(static_cast<unsigned char>(appId[i]));
            }
            if (same) return &s;
        }
        return nullptr;
    }
};

enum class ParlexFriendState { None, Friends, Sent, Received };

// ---------------------------------------------------------------------------
// Facades (declarations; definitions follow once every facade exists)
// ---------------------------------------------------------------------------

// Parlex - sign-in and every client API call.
struct ParlexClient {
    // Configuration (shared by every script).
    void Configure(const std::string& host, const std::string& titleId) const;
    void SetClientVersion(const std::string& version) const;  // sent as ClientVersion at sign-in
    std::string Host() const;
    std::string TitleId() const;
    std::string ClientVersion() const;
    std::string BaseUrl() const;

    // Session.
    std::string PlayerId() const;
    std::string DisplayName() const;
    std::string SessionTicket() const;
    bool LoggedIn() const;
    void Logout() const;

    // Sign-in.
    void LoginWithDevice(ParlexCallback done) const;
    void Login(const std::string& customId, ParlexCallback done) const;
    void LoginWithMeta(const std::string& metaUserId, const std::string& nonce, ParlexCallback done) const;
    void LoginWithSteam(const std::string& ticketHex, ParlexCallback done) const;
    void LoginWithGoogle(const std::string& idToken, ParlexCallback done) const;
    void LoginWithApple(const std::string& idToken, ParlexCallback done) const;
    void RegisterWithEmail(const std::string& email, const std::string& password, ParlexCallback done) const;
    void LoginWithEmail(const std::string& email, const std::string& password, ParlexCallback done) const;
    void Retry(ParlexCallback done) const;

    // Accounts.
    void LinkAccount(const std::string& provider, Json proof, ParlexCallback done) const;
    void LinkEmail(const std::string& email, const std::string& password, ParlexCallback done) const;
    void UnlinkAccount(const std::string& provider, ParlexCallback done) const;
    void ReportIntegrity(const std::string& signatureSha256, const std::string& version, bool debuggerAttached,
                         ParlexCallback done) const;
    void SetDisplayName(const std::string& name, ParlexCallback done) const;
    void SetAvatarUrl(const std::string& url, ParlexCallback done) const;
    void SetBio(const std::string& bio, ParlexCallback done) const;

    // Any client API by name: POST {BaseUrl}/Client/{api} with the session header.
    void Call(const std::string& api, Json args, ParlexCallback done) const;
    void ExecuteFunction(const std::string& name, Json args, ParlexCallback done) const;

    // Economy.
    void GetCatalog(ParlexCallback done) const { Call("GetCatalogItems", Json::Object(), std::move(done)); }
    void GetBalances(ParlexCallback done) const { Call("GetUserVirtualCurrency", Json::Object(), std::move(done)); }
    void GetInventory(ParlexCallback done) const { Call("GetUserInventory", Json::Object(), std::move(done)); }
    void PurchaseCart(const Json& items, const std::string& currency, int total, const std::string& giftToPlayerId,
                      const std::string& message, ParlexCallback done) const {
        Call("PurchaseCart", Json::Map({{"Items", items}, {"VirtualCurrency", currency}, {"Total", total},
                                        {"TargetPlayFabId", giftToPlayerId}, {"Message", message}}), std::move(done));
    }
    void PurchaseCart(const Json& items, const std::string& currency, int total, ParlexCallback done) const {
        Call("PurchaseCart", Json::Map({{"Items", items}, {"VirtualCurrency", currency}, {"Total", total}}), std::move(done));
    }
    void RedeemCoupon(const std::string& code, ParlexCallback done) const { Call("RedeemCoupon", Json::Map({{"Code", code}}), std::move(done)); }
    void Purchase(const std::string& itemId, const std::string& currency, ParlexCallback done) const {
        Call("PurchaseItem", Json::Map({{"ItemId", itemId}, {"VirtualCurrency", currency}}), std::move(done));
    }
    void ConsumeItem(const std::string& itemInstanceId, int count, ParlexCallback done) const {
        Call("ConsumeItem", Json::Map({{"ItemInstanceId", itemInstanceId}, {"ConsumeCount", count}}), std::move(done));
    }
    void GetTransactions(ParlexCallback done) const { Call("GetTransactionHistory", Json::Object(), std::move(done)); }

    // Messages, mutes, reports, rewards.
    void GetMessages(ParlexCallback done) const { Call("GetPlayerMessages", Json::Object(), std::move(done)); }
    void MarkMessageRead(int messageId, ParlexCallback done) const { Call("MarkMessageRead", Json::Map({{"MessageId", messageId}}), std::move(done)); }
    void GetMyMutes(ParlexCallback done) const { Call("GetMyMutes", Json::Object(), std::move(done)); }
    void AddMyMute(const std::string& playerId, ParlexCallback done) const { Call("AddMyMute", Json::Map({{"TargetPlayFabId", playerId}}), std::move(done)); }
    void RemoveMyMute(const std::string& playerId, ParlexCallback done) const { Call("RemoveMyMute", Json::Map({{"TargetPlayFabId", playerId}}), std::move(done)); }
    void GetMyReports(ParlexCallback done) const { Call("GetMyReports", Json::Object(), std::move(done)); }
    void GetDailyReward(ParlexCallback done) const { Call("GetDailyReward", Json::Object(), std::move(done)); }
    void ClaimDailyReward(ParlexCallback done) const { Call("ClaimDailyReward", Json::Object(), std::move(done)); }
    void ReportPlayer(const std::string& playerId, const std::string& reason, const std::string& room,
                      const std::string& note, ParlexCallback done) const {
        Call("SubmitReport", Json::Map({{"TargetPlayFabId", playerId}, {"Reason", reason.empty() ? std::string("other") : reason},
                                        {"Room", room}, {"Note", note}}), std::move(done));
    }

    // Friends.
    void GetFriends(ParlexCallback done) const { Call("GetFriendsList", Json::Object(), std::move(done)); }
    void AddFriend(const std::string& playerId, ParlexCallback done) const { Call("AddFriend", Json::Map({{"FriendPlayFabId", playerId}}), std::move(done)); }
    void RemoveFriend(const std::string& playerId, ParlexCallback done) const { Call("RemoveFriend", Json::Map({{"FriendPlayFabId", playerId}}), std::move(done)); }
    void SendFriendRequest(const std::string& playerId, ParlexCallback done) const { Call("SendFriendRequest", Json::Map({{"FriendPlayFabId", playerId}}), std::move(done)); }
    void GetFriendRequests(ParlexCallback done) const { Call("GetFriendRequests", Json::Object(), std::move(done)); }
    void AcceptFriendRequest(const std::string& playerId, ParlexCallback done) const { Call("AcceptFriendRequest", Json::Map({{"FriendPlayFabId", playerId}}), std::move(done)); }
    void DeclineFriendRequest(const std::string& playerId, ParlexCallback done) const { Call("DeclineFriendRequest", Json::Map({{"FriendPlayFabId", playerId}}), std::move(done)); }
    void CancelFriendRequest(const std::string& playerId, ParlexCallback done) const { Call("CancelFriendRequest", Json::Map({{"FriendPlayFabId", playerId}}), std::move(done)); }
    void GetPlayerCard(const std::string& playerId, ParlexCallback done) const { Call("GetPlayerCard", Json::Map({{"TargetPlayFabId", playerId}}), std::move(done)); }
    void GetPlayerPictures(const std::vector<std::string>& playerIds, ParlexCallback done) const { Call("GetPlayerPictures", Json::Map({{"Ids", Json(playerIds)}}), std::move(done)); }

    // Characters, groups, objects.
    void GetCharacters(ParlexCallback done) const { Call("GetAllUsersCharacters", Json::Object(), std::move(done)); }
    void GetCharacterData(const std::string& characterId, ParlexCallback done) const { Call("GetCharacterData", Json::Map({{"CharacterId", characterId}}), std::move(done)); }
    void UpdateCharacterData(const std::string& characterId, const Json& data, ParlexCallback done) const {
        Call("UpdateCharacterData", Json::Map({{"CharacterId", characterId}, {"Data", data}}), std::move(done));
    }
    void GetGroups(ParlexCallback done) const { Call("ListMembership", Json::Object(), std::move(done)); }
    void GetPlayerObjects(ParlexCallback done) const { Call("GetPlayerObjects", Json::Object(), std::move(done)); }
    void SetPlayerObject(const std::string& name, const Json& value, ParlexCallback done) const {
        Call("SetPlayerObject", Json::Map({{"Name", name}, {"Value", value.Dump()}}), std::move(done));
    }
    void GetTitleObjects(ParlexCallback done) const { Call("GetTitleObjects", Json::Object(), std::move(done)); }
    void GetTitleData(const std::vector<std::string>& keys, ParlexCallback done) const { Call("GetTitleData", Json::Map({{"Keys", Json(keys)}}), std::move(done)); }

    // Experiments, matchmaking, leaderboards.
    void GetExperiment(const std::string& experimentId, ParlexCallback done) const { Call("GetExperimentVariant", Json::Map({{"Experiment", experimentId}}), std::move(done)); }
    void CreateMatchmakingTicket(const std::string& queue, int rating, ParlexCallback done) const { Call("CreateMatchmakingTicket", Json::Map({{"Queue", queue}, {"Rating", rating}}), std::move(done)); }
    void GetMatchmakingTicket(const std::string& ticketId, ParlexCallback done) const { Call("GetMatchmakingTicket", Json::Map({{"TicketId", ticketId}}), std::move(done)); }
    void CancelMatchmakingTicket(const std::string& ticketId, ParlexCallback done) const { Call("CancelMatchmakingTicket", Json::Map({{"TicketId", ticketId}}), std::move(done)); }
    void GetLeaderboard(const std::string& statistic, int start, int count, ParlexCallback done) const {
        Call("GetLeaderboard", Json::Map({{"StatisticName", statistic}, {"StartPosition", start}, {"MaxResultsCount", count}}), std::move(done));
    }
    void GetLeaderboardAroundMe(const std::string& statistic, int count, ParlexCallback done) const {
        Call("GetLeaderboardAroundUser", Json::Map({{"StatisticName", statistic}, {"MaxResultsCount", count}}), std::move(done));
    }

    // Discord.
    void CreateDiscordLinkCode(ParlexCallback done) const { Call("CreateDiscordLinkCode", Json::Object(), std::move(done)); }
    void GetDiscordLink(ParlexCallback done) const { Call("GetDiscordLink", Json::Object(), std::move(done)); }
    void UnlinkDiscord(ParlexCallback done) const { Call("UnlinkDiscord", Json::Object(), std::move(done)); }

    // Servers and private rooms.
    void GetServerStatus(ParlexCallback done) const { Call("GetServerStatus", Json::Object(), std::move(done)); }
    void PickServer(const std::string& product, const std::string& region, ParlexCallback done) const { Call("PickServer", Json::Map({{"Product", product}, {"Region", region}}), std::move(done)); }
    void ServerPresence(const std::string& serverId, ParlexCallback done) const { Call("ServerPresence", Json::Map({{"ServerId", serverId}}), std::move(done)); }
    void ServerPresence(const std::string& serverId, const std::string& room, ParlexCallback done) const { Call("ServerPresence", Json::Map({{"ServerId", serverId}, {"Room", room}}), std::move(done)); }
    void LeaveServer(ParlexCallback done) const { Call("LeaveServer", Json::Object(), std::move(done)); }
    void OpenRooms(ParlexCallback done) const { Call("OpenRooms", Json::Object(), std::move(done)); }
    void PrivateCheck(const std::string& room, ParlexCallback done) const { Call("PrivateCheck", Json::Map({{"Room", room}}), std::move(done)); }
    void PrivateMine(ParlexCallback done) const { Call("PrivateMine", Json::Object(), std::move(done)); }
    void PrivateCreate(const std::string& name, const std::string& room, bool inviteOnly, ParlexCallback done) const {
        Call("PrivateCreate", Json::Map({{"Name", name}, {"Room", room}, {"Mode", inviteOnly ? "whitelist" : "open"}}), std::move(done));
    }
    void PrivateSetOpen(const std::string& privateId, bool open, ParlexCallback done) const { Call("PrivateSetOpen", Json::Map({{"PrivateId", privateId}, {"Open", open}}), std::move(done)); }
    void PrivateAllow(const std::string& privateId, const std::string& playerId, ParlexCallback done) const {
        Call("PrivateListAdd", Json::Map({{"PrivateId", privateId}, {"PlayFabIds", playerId}, {"Kind", "allow"}}), std::move(done));
    }
    void PrivateBlock(const std::string& privateId, const std::string& playerId, ParlexCallback done) const {
        Call("PrivateListAdd", Json::Map({{"PrivateId", privateId}, {"PlayFabIds", playerId}, {"Kind", "block"}}), std::move(done));
    }
    void PrivateRemove(const std::string& privateId, const std::string& playerId, ParlexCallback done) const {
        Call("PrivateListRemove", Json::Map({{"PrivateId", privateId}, {"TargetPlayFabId", playerId}}), std::move(done));
    }
    void PrivateList(const std::string& privateId, ParlexCallback done) const { Call("PrivateGetList", Json::Map({{"PrivateId", privateId}}), std::move(done)); }
    void PrivateDelete(const std::string& privateId, ParlexCallback done) const { Call("PrivateDelete", Json::Map({{"PrivateId", privateId}}), std::move(done)); }
    void PrivateSetMode(const std::string& privateId, bool inviteOnly, ParlexCallback done) const {
        Call("PrivateSetMode", Json::Map({{"PrivateId", privateId}, {"Mode", inviteOnly ? "whitelist" : "open"}}), std::move(done));
    }

    // Low-level: POST {BaseUrl}{path}. `authed` adds X-Parlex-Session.
    void Send(const std::string& path, const Json& body, bool authed, ParlexCallback done,
              const HttpHeaders& extraHeaders = HttpHeaders(), int timeoutSeconds = 20) const;

private:
    void SignIn(const std::string& path, Json body, ParlexCallback done) const;
};

// ParlexBan - the local player's ban, filled from AccountBanned responses.
struct ParlexBanFacade {
    using BannedFn = std::function<void(const std::string& reason, bool hasExpiry, double expiresUnix)>;

    bool IsBanned() const { return parlex_detail::Read("ban.active") == "1"; }
    std::string Reason() const { return parlex_detail::Read("ban.reason"); }
    bool HasExpiry() const { return parlex_detail::Exists("ban.expires") && !parlex_detail::Read("ban.expires").empty(); }
    double ExpiresUnix() const { return std::strtod(parlex_detail::Read("ban.expires", "0").c_str(), nullptr); }
    bool HasStart() const { return parlex_detail::Exists("ban.started") && !parlex_detail::Read("ban.started").empty(); }
    double StartedUnix() const { return std::strtod(parlex_detail::Read("ban.started", "0").c_str(), nullptr); }
    bool Permanent() const { return IsBanned() && !HasExpiry(); }
    // Seconds left; +infinity for permanent bans, 0 when not banned or over.
    double RemainingSeconds() const;

    std::string Countdown() const;
    std::string LengthText() const;
    std::string EndsText() const;
    std::string Describe() const;

    // Listener fired when the ban first arrives or changes. Returns a handle for OffBanned.
    int OnBanned(BannedFn fn) const;
    void OffBanned(int handle) const;

    void Notify(const ParlexResult& r) const;
    void Clear() const;
};

// ParlexServerGate - maintenance / per-server status, kicks and mutes.
struct ParlexServerGateFacade {
    using ChangedFn = std::function<void(const ParlexStatus&)>;
    using KickedFn = std::function<void(const std::string& reason)>;
    using MuteChangedFn = std::function<void(bool muted, const std::string& reason)>;

    // Latest status (parsed once per change; shared by every script).
    const ParlexStatus& Current() const;
    // Increments whenever the status is replaced; poll it from scripts that did not
    // register listeners in the polling module.
    unsigned int Revision() const { return parlex_detail::Revision("gate.status"); }
    bool Muted() const { return parlex_detail::Read("gate.muted") == "1"; }

    void NotifyClosed(const std::string& message) const;
    bool IsRoomLocked(const std::string& room) const;
    bool HostLeaveAll() const;
    bool IsOpen(const std::string& serverId, std::string& message) const;
    bool IsOpen(const std::string& serverId = std::string()) const { std::string m; return IsOpen(serverId, m); }
    // The text Unity's OnGUI banner shows; empty when everything is up.
    std::string BannerText() const;

    // Call every frame from one script (ParlexBootstrap does). Polls GetServerStatus
    // every `pollSeconds` (min 3) while signed in.
    void Poll(float deltaSeconds, float pollSeconds = 10.0f) const;
    // Fetch now, regardless of the poll timer.
    void Refresh(ParlexCallback done = ParlexCallback()) const;

    int OnChanged(ChangedFn fn) const;
    int OnKicked(KickedFn fn) const;
    int OnMuteChanged(MuteChangedFn fn) const;
    void Off(int handle) const;

    void Publish(const Json& statusData) const;
    static ParlexStatus Parse(const Json& data);
};

// ParlexFriends - cached friends / requests with optimistic updates (per script).
struct ParlexFriendsFacade {
    using ChangedFn = std::function<void()>;
    using RequestFn = std::function<void(const std::string& playerId, const std::string& displayName)>;

    bool Loaded() const;
    bool Is(const std::string& id) const { return State(id) == ParlexFriendState::Friends; }
    ParlexFriendState State(const std::string& id) const;
    std::vector<std::pair<std::string, std::string>> Friends() const;
    std::vector<std::pair<std::string, std::string>> Incoming() const;
    std::vector<std::pair<std::string, std::string>> Outgoing() const;
    void Refresh(ParlexCallback done = ParlexCallback()) const;
    // Friend ids the server reports as online (empty if the reply carries no presence).
    std::vector<std::string> Online() const;
    std::string NameOf(const std::string& id) const;
    // True when signed in, idle, and the last load is at least `everySeconds` old.
    bool Due(float everySeconds) const;
    void Request(const std::string& id, const std::string& name, std::function<void()> done = {}) const;
    void Accept(const std::string& id, std::function<void()> done = {}) const;
    void Decline(const std::string& id, std::function<void()> done = {}) const;
    void Cancel(const std::string& id, std::function<void()> done = {}) const;
    void Remove(const std::string& id, std::function<void()> done = {}) const;
    int OnChanged(ChangedFn fn) const;
    int OnRequestArrived(RequestFn fn) const;
    void Off(int handle) const;
};

// ParlexSavedMutes - the player's saved mute list (per script).
struct ParlexSavedMutesFacade {
    bool Loaded() const;
    bool Contains(const std::string& id) const;
    std::vector<std::string> All() const;
    void Load(std::function<void()> done = {}) const;
    void Set(const std::string& id, bool on) const;
};

// ParlexIntegrity - build integrity report (signature is Android-only in Parlex).
struct ParlexIntegrityFacade {
    std::string SigningHash() const { return std::string(); }
    bool DebuggerAttached() const;
    void Report(ParlexCallback done = ParlexCallback()) const;
};

// ParlexErrorReporter - forward error text to the Parlex console (max 10 per session,
// deduplicated). Unity hooked Application.logMessageReceived; here call Report().
struct ParlexErrorReporterFacade {
    void Report(const std::string& message, const std::string& stack = std::string()) const;
    void Flush() const;
};

// ParlexServerAgent - dedicated-server heartbeat (needs -parlexKey <key> or PARLEX_KEY).
struct ParlexServerAgentFacade {
    std::string FindKey() const;
    std::string Command() const { return parlex_detail::Read("agent.command"); }
    // Sends one heartbeat; done receives the result (Command is in r.GetString("Command")).
    void Heartbeat(const std::string& serverId, int players, int maxPlayers, ParlexCallback done = ParlexCallback()) const;
};

inline const ParlexClient Parlex{};
inline const ParlexBanFacade ParlexBan{};
inline const ParlexServerGateFacade ParlexServerGate{};
inline const ParlexFriendsFacade ParlexFriends{};
inline const ParlexSavedMutesFacade ParlexSavedMutes{};
inline const ParlexIntegrityFacade ParlexIntegrity{};
inline const ParlexErrorReporterFacade ParlexErrorReporter{};
inline const ParlexServerAgentFacade ParlexServerAgent{};

// ---------------------------------------------------------------------------
// Module-local state
// ---------------------------------------------------------------------------
namespace parlex_detail {

struct ModuleState {
    std::string epoch;
    int nextHandle = 1;
    std::map<int, ParlexBanFacade::BannedFn> banned;
    std::map<int, ParlexServerGateFacade::ChangedFn> gateChanged;
    std::map<int, ParlexServerGateFacade::KickedFn> gateKicked;
    std::map<int, ParlexServerGateFacade::MuteChangedFn> gateMuteChanged;
    std::map<int, ParlexFriendsFacade::ChangedFn> friendsChanged;
    std::map<int, ParlexFriendsFacade::RequestFn> friendsRequest;

    // Status cache.
    unsigned int statusRevision = std::numeric_limits<unsigned int>::max();
    ParlexStatus status;
    float pollTimer = 0.0f;
    bool pollInFlight = false;

    // Friends cache.
    std::map<std::string, std::string> friends, sent, received;
    std::set<std::string> friendsOnline;
    std::string friendsOwner;
    bool friendsLoading = false;
    bool friendsLoaded = false;
    double friendsLastLoad = 0.0;

    // Saved mutes cache.
    std::set<std::string> mutes;
    std::string mutesOwner;
    bool mutesLoading = false;
    bool mutesLoaded = false;

    // Error reporter.
    std::set<std::string> errorsSeen;
    std::vector<std::pair<std::string, std::string>> errorQueue;
};

inline ModuleState& Module() {
    static ModuleState state;
    ScriptContext* c = ctxPtr();
    if (!c) return state;
    std::string epoch = c->GetSharedValue("parlex.epoch");
    if (epoch.empty()) {
        std::random_device rd;
        epoch = std::to_string(rd()) + "-" + std::to_string(static_cast<long long>(NowUnix() * 1000.0));
        c->SetSharedValue("parlex.epoch", epoch);
    }
    if (state.epoch != epoch) {
        state = ModuleState();  // new play session: drop listeners/caches from the last one
        state.epoch = epoch;
    }
    return state;
}

template <typename Map, typename... Args>
inline void Fire(const Map& listeners, Args&&... args) {
    // Copy so a listener may add/remove listeners while being called.
    const Map copy = listeners;
    for (const auto& entry : copy) {
        if (entry.second) entry.second(args...);
    }
}

}  // namespace parlex_detail

// ---------------------------------------------------------------------------
// ParlexClient
// ---------------------------------------------------------------------------
inline void ParlexClient::Configure(const std::string& host, const std::string& titleId) const {
    parlex_detail::Write("host", host);
    parlex_detail::Write("title", titleId);
}
inline void ParlexClient::SetClientVersion(const std::string& version) const { parlex_detail::Write("clientVersion", version); }
inline std::string ParlexClient::Host() const {
    const std::string h = parlex_detail::Read("host");
    return h.empty() ? std::string(parlex_detail::kDefaultHost) : h;
}
inline std::string ParlexClient::TitleId() const { return parlex_detail::Read("title"); }
inline std::string ParlexClient::ClientVersion() const { return parlex_detail::Read("clientVersion", "1.0"); }
inline std::string ParlexClient::BaseUrl() const {
    std::string host = Host();
    while (!host.empty() && host.back() == '/') host.pop_back();
    return host + "/t/" + TitleId();
}
inline std::string ParlexClient::PlayerId() const { return parlex_detail::Read("playerId"); }
inline std::string ParlexClient::DisplayName() const { return parlex_detail::Read("displayName"); }
inline std::string ParlexClient::SessionTicket() const { return parlex_detail::Read("session"); }
inline bool ParlexClient::LoggedIn() const { return !parlex_detail::Read("session").empty(); }
inline void ParlexClient::Logout() const {
    parlex_detail::Write("session", "");
    parlex_detail::Write("playerId", "");
}

inline void ParlexClient::Send(const std::string& path, const Json& body, bool authed, ParlexCallback done,
                               const HttpHeaders& extraHeaders, int timeoutSeconds) const {
    if (!ctxPtr()) {
        if (done) done(ParlexResult::Failure("NoContext", "Parlex was called outside a running script"));
        return;
    }
    if (TitleId().empty()) {
        if (done) done(ParlexResult::Failure("NotConfigured", "Call Parlex.Configure(host, titleId) first"));
        return;
    }
    HttpHeaders headers = extraHeaders;
    if (authed) headers.emplace_back("X-Parlex-Session", SessionTicket());
    Http.PostJson(BaseUrl() + path, body, [authed, done](const HttpResponse& response) {
        std::string netError = response.Error;
        if (netError.empty() && (response.Status < 200 || response.Status >= 300)) {
            netError = "HTTP error " + std::to_string(response.Status);
        }
        ParlexResult res = ParlexResult::FromResponse(response.Status, response.Body, netError);
        if (!res.Ok && res.Code == 401) parlex_detail::Write("session", "");
        if (!res.Ok && res.ErrorName == "AccountBanned") {
            if (authed) parlex_detail::Write("session", "");
            ParlexBan.Notify(res);
        }
        if (done) done(res);
    }, headers, timeoutSeconds);
}

inline void ParlexClient::SignIn(const std::string& path, Json body, ParlexCallback done) const {
    body["ClientVersion"] = ClientVersion();
    parlex_detail::Write("lastPath", path);
    parlex_detail::Write("lastBody", body.Dump());
    Send(path, body, false, [done](const ParlexResult& r) {
        if (r.Ok) {
            parlex_detail::Write("session", r.GetString("SessionTicket"));
            parlex_detail::Write("playerId", r.GetString("PlayerId"));
            ParlexBan.Clear();
            ParlexErrorReporter.Flush();
        } else if (r.ErrorName == "MaintenanceMode") {
            ParlexServerGate.NotifyClosed(r.Error);
        }
        if (done) done(r);
    });
}

inline void ParlexClient::LoginWithDevice(ParlexCallback done) const {
    Login("device:" + Http.DeviceId(), std::move(done));
}
inline void ParlexClient::Login(const std::string& customId, ParlexCallback done) const {
    SignIn("/Client/LoginWithCustomID", Json::Map({{"CustomId", customId}}), std::move(done));
}
inline void ParlexClient::LoginWithMeta(const std::string& metaUserId, const std::string& nonce, ParlexCallback done) const {
    SignIn("/Client/LoginWithMeta", Json::Map({{"MetaId", metaUserId}, {"Nonce", nonce}}), std::move(done));
}
inline void ParlexClient::LoginWithSteam(const std::string& ticketHex, ParlexCallback done) const {
    SignIn("/Client/LoginWithSteam", Json::Map({{"Ticket", ticketHex}}), std::move(done));
}
inline void ParlexClient::LoginWithGoogle(const std::string& idToken, ParlexCallback done) const {
    SignIn("/Client/LoginWithGoogle", Json::Map({{"IdToken", idToken}}), std::move(done));
}
inline void ParlexClient::LoginWithApple(const std::string& idToken, ParlexCallback done) const {
    SignIn("/Client/LoginWithApple", Json::Map({{"IdToken", idToken}}), std::move(done));
}
inline void ParlexClient::RegisterWithEmail(const std::string& email, const std::string& password, ParlexCallback done) const {
    SignIn("/Client/RegisterWithEmail", Json::Map({{"Email", email}, {"Password", password}}), std::move(done));
}
inline void ParlexClient::LoginWithEmail(const std::string& email, const std::string& password, ParlexCallback done) const {
    SignIn("/Client/LoginWithEmail", Json::Map({{"Email", email}, {"Password", password}}), std::move(done));
}
inline void ParlexClient::Retry(ParlexCallback done) const {
    const std::string path = parlex_detail::Read("lastPath");
    if (path.empty()) {
        if (done) done(ParlexResult::Failure("NotSignedIn", "Nothing to sign in with yet"));
        return;
    }
    SignIn(path, Json::Parse(parlex_detail::Read("lastBody")), std::move(done));
}

inline void ParlexClient::Call(const std::string& api, Json args, ParlexCallback done) const {
    if (!LoggedIn()) {
        if (done) done(ParlexResult::Failure("NotSignedIn", "Sign in first"));
        return;
    }
    if (!args.IsObject()) args = Json::Object();
    Send("/Client/" + api, args, true, std::move(done));
}

inline void ParlexClient::LinkAccount(const std::string& provider, Json proof, ParlexCallback done) const {
    proof["Provider"] = provider;
    Call("LinkAccount", std::move(proof), std::move(done));
}
inline void ParlexClient::LinkEmail(const std::string& email, const std::string& password, ParlexCallback done) const {
    LinkAccount("email", Json::Map({{"Email", email}, {"Password", password}}), std::move(done));
}
inline void ParlexClient::UnlinkAccount(const std::string& provider, ParlexCallback done) const {
    Call("UnlinkAccount", Json::Map({{"Provider", provider}}), std::move(done));
}
inline void ParlexClient::ReportIntegrity(const std::string& signatureSha256, const std::string& version,
                                          bool debuggerAttached, ParlexCallback done) const {
    Call("ReportIntegrity", Json::Map({{"Signature", signatureSha256}, {"Version", version}, {"Debugger", debuggerAttached}}),
         std::move(done));
}
inline void ParlexClient::SetDisplayName(const std::string& name, ParlexCallback done) const {
    Call("UpdateUserTitleDisplayName", Json::Map({{"DisplayName", name}}), [name, done](const ParlexResult& r) {
        if (r.Ok) parlex_detail::Write("displayName", name);
        if (done) done(r);
    });
}
inline void ParlexClient::SetAvatarUrl(const std::string& url, ParlexCallback done) const {
    Call("SetPlayerAvatar", Json::Map({{"Url", url}}), std::move(done));
}
inline void ParlexClient::SetBio(const std::string& bio, ParlexCallback done) const {
    Call("SetMyBio", Json::Map({{"Bio", bio}}), std::move(done));
}
inline void ParlexClient::ExecuteFunction(const std::string& name, Json args, ParlexCallback done) const {
    if (!args.IsObject()) args = Json::Object();
    Call("ExecuteCloudScript", Json::Map({{"FunctionName", name}, {"FunctionParameter", args}}), std::move(done));
}

// ---------------------------------------------------------------------------
// ParlexBan
// ---------------------------------------------------------------------------
inline double ParlexBanFacade::RemainingSeconds() const {
    if (!IsBanned()) return 0.0;
    if (!HasExpiry()) return std::numeric_limits<double>::infinity();
    const double left = ExpiresUnix() - parlex_detail::NowUnix();
    return left > 0.0 ? left : 0.0;
}

inline std::string ParlexBanFacade::Countdown() const {
    if (!IsBanned()) return "";
    if (!HasExpiry()) return "Permanent";
    const double left = RemainingSeconds();
    if (left <= 0.0) return "00:00:00";
    const long long total = static_cast<long long>(left);
    const long long days = total / 86400;
    const std::string clock = parlex_detail::TwoDigits((total / 3600) % 24) + ":" +
                              parlex_detail::TwoDigits((total / 60) % 60) + ":" + parlex_detail::TwoDigits(total % 60);
    return days > 0 ? std::to_string(days) + "d " + clock : clock;
}

inline std::string ParlexBanFacade::LengthText() const {
    if (!IsBanned()) return "";
    if (!HasExpiry()) return "You are banned for good";
    if (!HasStart()) return "You are banned";
    const double total = ExpiresUnix() - StartedUnix();
    const double minutes = total / 60.0, hours = total / 3600.0, days = total / 86400.0;
    if (minutes < 1.0) return "You are banned for under a minute";
    std::string span;
    if (days >= 1.5) span = std::to_string(static_cast<long long>(std::llround(days))) + " days";
    else if (hours >= 1.5) span = std::to_string(static_cast<long long>(std::llround(hours))) + " hours";
    else if (hours >= 0.99) span = "1 hour";
    else span = std::to_string(static_cast<long long>(std::llround(minutes))) + " minutes";
    return "You are banned for " + span;
}

inline std::string ParlexBanFacade::EndsText() const {
    if (!IsBanned()) return "";
    if (!HasExpiry()) return "This ban does not end.";
    if (RemainingSeconds() <= 0.0) return "The ban has ended. You can try again.";
    const std::time_t when = static_cast<std::time_t>(ExpiresUnix());
    const std::tm* local = std::localtime(&when);
    if (!local) return "Ends soon";
    static const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const int hour12 = local->tm_hour % 12 == 0 ? 12 : local->tm_hour % 12;
    return std::string("Ends ") + kDays[local->tm_wday] + " " + std::to_string(local->tm_mday) + " " +
           kMonths[local->tm_mon] + ", " + std::to_string(hour12) + ":" + parlex_detail::TwoDigits(local->tm_min) +
           (local->tm_hour < 12 ? " AM" : " PM");
}

inline std::string ParlexBanFacade::Describe() const {
    if (!IsBanned()) return "";
    if (!HasExpiry()) return "This ban does not end.";
    const double left = RemainingSeconds();
    if (left <= 0.0) return "The ban has ended. You can try again.";
    const long long total = static_cast<long long>(left);
    if (left >= 86400.0) {
        const long long d = total / 86400;
        return "The ban ends in " + std::to_string(d) + (d == 1 ? " day " : " days ") + std::to_string((total / 3600) % 24) + "h.";
    }
    if (left >= 3600.0) return "The ban ends in " + std::to_string(total / 3600) + "h " + std::to_string((total / 60) % 60) + "m.";
    const long long minutes = std::max<long long>(1, static_cast<long long>(std::ceil(left / 60.0)));
    return "The ban ends in " + std::to_string(minutes) + " minutes.";
}

inline int ParlexBanFacade::OnBanned(BannedFn fn) const {
    auto& m = parlex_detail::Module();
    const int handle = m.nextHandle++;
    m.banned[handle] = std::move(fn);
    return handle;
}
inline void ParlexBanFacade::OffBanned(int handle) const { parlex_detail::Module().banned.erase(handle); }

inline void ParlexBanFacade::Notify(const ParlexResult& r) const {
    double expires = 0.0, started = 0.0;
    const bool hasExpiry = parlex_detail::ParseTime(r.BanExpires, expires);
    const bool hasStart = parlex_detail::ParseTime(r.BanStarted, started);
    const std::string reason = r.BanReason.empty() ? std::string("Banned") : r.BanReason;
    const std::string expiresText = hasExpiry ? Json::FormatNumber(expires) : std::string();
    const bool changed = !IsBanned() || Reason() != reason || parlex_detail::Read("ban.expires") != expiresText;
    parlex_detail::Write("ban.active", "1");
    parlex_detail::Write("ban.reason", reason);
    parlex_detail::Write("ban.expires", expiresText);
    parlex_detail::Write("ban.started", hasStart ? Json::FormatNumber(started) : std::string());
    if (changed) parlex_detail::Fire(parlex_detail::Module().banned, reason, hasExpiry, expires);
}

inline void ParlexBanFacade::Clear() const {
    parlex_detail::Write("ban.active", "");
    parlex_detail::Write("ban.reason", "");
    parlex_detail::Write("ban.expires", "");
    parlex_detail::Write("ban.started", "");
}

// ---------------------------------------------------------------------------
// ParlexServerGate
// ---------------------------------------------------------------------------
inline ParlexStatus ParlexServerGateFacade::Parse(const Json& d) {
    ParlexStatus s;
    s.GameOpen = d.Get("GameOpen").AsBool(true);
    s.Message = d.Get("Message").AsString();
    s.KickToken = d.Get("Kick.Token").AsString();
    s.KickReason = d.Get("Kick.Reason").AsString();
    s.Muted = d.Get("Muted").AsBool(false);
    s.MuteReason = d.Get("MuteReason").AsString();
    s.AnnouncementId = d.Get("Announcement.Id").AsString();
    s.AnnouncementText = d.Get("Announcement.Text").AsString();
    s.AnnouncementFrom = d.Get("Announcement.From").AsString();
    s.AnnouncementOnce = d.Get("Announcement.Once").AsBool(false);
    s.MuteEnds = d.Get("MuteEnds").AsString();
    for (const Json& o : d["LockedRooms"].Items()) s.LockedRooms.push_back(o.AsString());
    for (const Json& o : d["ShadowMuted"].Items()) s.ShadowMuted.push_back(o.AsString());
    for (const Json& o : d["Servers"].Items()) {
        if (!o.IsObject()) continue;
        ParlexServerInfo info;
        info.Id = o["ServerId"].AsString();
        info.Name = o["Name"].AsString();
        info.Kind = o["Kind"].AsString();
        info.Product = o["Product"].AsString();
        info.AppId = o["AppId"].AsString();
        info.Region = o["Region"].AsString();
        info.Status = o["Status"].AsString();
        info.Message = o["Message"].AsString();
        info.Address = o["Address"].AsString();
        info.Players = o["Players"].AsInt(0);
        info.MaxPlayers = o["MaxPlayers"].AsInt(0);
        const std::string whitelist = o["Whitelist"].AsString();
        const std::string canJoin = o["CanJoin"].AsString();
        const std::string hostLeaveAll = o["HostLeaveAll"].AsString();
        info.InviteOnly = whitelist == "True" || whitelist == "true";
        info.CanJoin = canJoin != "False" && canJoin != "false";
        info.HostLeaveAll = hostLeaveAll == "True" || hostLeaveAll == "true";
        s.Servers.push_back(std::move(info));
    }
    return s;
}

inline const ParlexStatus& ParlexServerGateFacade::Current() const {
    auto& m = parlex_detail::Module();
    const unsigned int revision = Revision();
    if (m.statusRevision != revision) {
        m.statusRevision = revision;
        const std::string raw = parlex_detail::Read("gate.status");
        m.status = raw.empty() ? ParlexStatus() : Parse(Json::Parse(raw));
    }
    return m.status;
}

inline void ParlexServerGateFacade::Publish(const Json& statusData) const {
    parlex_detail::Write("gate.status", statusData.Dump());
    // A copy: listeners may publish again, which replaces the cached status.
    const ParlexStatus s = Current();
    auto& m = parlex_detail::Module();
    if (!parlex_detail::Exists("gate.lastKick")) {
        parlex_detail::Write("gate.lastKick", s.KickToken);
    } else if (!s.KickToken.empty() && s.KickToken != parlex_detail::Read("gate.lastKick")) {
        parlex_detail::Write("gate.lastKick", s.KickToken);
        const std::string reason = s.KickReason.empty() ? std::string("You were removed from the server.") : s.KickReason;
        parlex_detail::Fire(m.gateKicked, reason);
    }
    if (s.Muted != Muted()) {
        parlex_detail::Write("gate.muted", s.Muted ? "1" : "");
        parlex_detail::Fire(m.gateMuteChanged, s.Muted, s.MuteReason);
    }
    parlex_detail::Fire(m.gateChanged, s);
}

inline void ParlexServerGateFacade::NotifyClosed(const std::string& message) const {
    Publish(Json::Map({{"GameOpen", false}, {"Message", message}}));
}

inline bool ParlexServerGateFacade::IsRoomLocked(const std::string& room) const {
    if (room.empty()) return false;
    auto lower = [](std::string v) {
        for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return v;
    };
    const std::string wanted = lower(room);
    for (const auto& r : Current().LockedRooms) if (lower(r) == wanted) return true;
    return false;
}

inline bool ParlexServerGateFacade::HostLeaveAll() const {
    for (const auto& s : Current().Servers) {
        if (s.HostLeaveAll && s.IsOnline() && (s.Product == "pun" || s.Kind == "photon")) return true;
    }
    return false;
}

inline bool ParlexServerGateFacade::IsOpen(const std::string& serverId, std::string& message) const {
    message.clear();
    const ParlexStatus& s = Current();
    if (!s.GameOpen) {
        message = s.Message.empty() ? std::string("The game is down for maintenance.") : s.Message;
        return false;
    }
    if (serverId.empty()) return true;
    const ParlexServerInfo* sv = s.Find(serverId);
    if (!sv || sv->IsOnline()) return true;
    message = !sv->Message.empty() ? sv->Message
              : (sv->Status == "maintenance" ? sv->Name + " is in maintenance." : sv->Name + " is offline.");
    return false;
}

inline std::string ParlexServerGateFacade::BannerText() const {
    const ParlexStatus& s = Current();
    if (!s.GameOpen) return s.Message.empty() ? std::string("The game is down for maintenance.") : s.Message;
    for (const auto& sv : s.Servers) {
        if (sv.IsOnline()) continue;
        return sv.Name + (sv.Status == "maintenance" ? " is in maintenance" : " is offline") +
               (sv.Message.empty() ? std::string() : ": " + sv.Message);
    }
    return std::string();
}

inline void ParlexServerGateFacade::Refresh(ParlexCallback done) const {
    auto& m = parlex_detail::Module();
    m.pollInFlight = true;
    Parlex.GetServerStatus([done](const ParlexResult& r) {
        parlex_detail::Module().pollInFlight = false;
        if (r.Ok) ParlexServerGate.Publish(r.Data);
        if (done) done(r);
    });
}

inline void ParlexServerGateFacade::Poll(float deltaSeconds, float pollSeconds) const {
    auto& m = parlex_detail::Module();
    m.pollTimer -= deltaSeconds;
    if (m.pollTimer > 0.0f || m.pollInFlight) return;
    m.pollTimer = pollSeconds < 3.0f ? 3.0f : pollSeconds;
    if (Parlex.LoggedIn()) Refresh();
}

inline int ParlexServerGateFacade::OnChanged(ChangedFn fn) const {
    auto& m = parlex_detail::Module();
    const int h = m.nextHandle++;
    m.gateChanged[h] = std::move(fn);
    return h;
}
inline int ParlexServerGateFacade::OnKicked(KickedFn fn) const {
    auto& m = parlex_detail::Module();
    const int h = m.nextHandle++;
    m.gateKicked[h] = std::move(fn);
    return h;
}
inline int ParlexServerGateFacade::OnMuteChanged(MuteChangedFn fn) const {
    auto& m = parlex_detail::Module();
    const int h = m.nextHandle++;
    m.gateMuteChanged[h] = std::move(fn);
    return h;
}
inline void ParlexServerGateFacade::Off(int handle) const {
    auto& m = parlex_detail::Module();
    m.gateChanged.erase(handle);
    m.gateKicked.erase(handle);
    m.gateMuteChanged.erase(handle);
}

// ---------------------------------------------------------------------------
// ParlexFriends
// ---------------------------------------------------------------------------
inline std::vector<std::string> ParlexFriendsFacade::Online() const {
    const auto& s = parlex_detail::Module().friendsOnline;
    return std::vector<std::string>(s.begin(), s.end());
}
inline std::string ParlexFriendsFacade::NameOf(const std::string& id) const {
    const auto& m = parlex_detail::Module().friends;
    const auto it = m.find(id);
    return it == m.end() ? std::string() : it->second;
}
inline bool ParlexFriendsFacade::Loaded() const { return parlex_detail::Module().friendsLoaded; }

inline ParlexFriendState ParlexFriendsFacade::State(const std::string& id) const {
    if (id.empty()) return ParlexFriendState::None;
    auto& m = parlex_detail::Module();
    if (Parlex.LoggedIn() && (!m.friendsLoaded || m.friendsOwner != Parlex.PlayerId()) && !m.friendsLoading) Refresh();
    if (m.friends.count(id)) return ParlexFriendState::Friends;
    if (m.received.count(id)) return ParlexFriendState::Received;
    if (m.sent.count(id)) return ParlexFriendState::Sent;
    return ParlexFriendState::None;
}

inline std::vector<std::pair<std::string, std::string>> ParlexFriendsFacade::Friends() const {
    const auto& m = parlex_detail::Module().friends;
    return std::vector<std::pair<std::string, std::string>>(m.begin(), m.end());
}
inline std::vector<std::pair<std::string, std::string>> ParlexFriendsFacade::Incoming() const {
    const auto& m = parlex_detail::Module().received;
    return std::vector<std::pair<std::string, std::string>>(m.begin(), m.end());
}
inline std::vector<std::pair<std::string, std::string>> ParlexFriendsFacade::Outgoing() const {
    const auto& m = parlex_detail::Module().sent;
    return std::vector<std::pair<std::string, std::string>>(m.begin(), m.end());
}

inline void ParlexFriendsFacade::Refresh(ParlexCallback done) const {
    auto& m = parlex_detail::Module();
    if (m.friendsLoading || !Parlex.LoggedIn()) return;
    const std::string me = Parlex.PlayerId();
    if (m.friendsOwner != me) {
        m.friends.clear();
        m.sent.clear();
        m.received.clear();
        m.friendsLoaded = false;
        m.friendsOwner = me;
    }
    m.friendsLoading = true;
    m.friendsLastLoad = parlex_detail::NowUnix();
    Parlex.GetFriends([done](const ParlexResult& r) {
        auto& m1 = parlex_detail::Module();
        if (r.Ok) {
            m1.friends.clear();
            m1.friendsOnline.clear();
            for (const Json& o : r.GetList("Friends")) {
                const std::string id = o["FriendPlayFabId"].AsString();
                if (id.empty()) continue;
                m1.friends[id] = o["DisplayName"].AsString();
                // Presence fields are optional in the Parlex reply; accept the usual spellings.
                bool online = o["Online"].AsBool(false) || o["IsOnline"].AsBool(false) ||
                              parlex_detail::Lower(o["Status"].AsString()) == "online";
                double seen = 0.0;
                if (!online && parlex_detail::ParseTime(o["LastLogin"].AsString(), seen) &&
                    parlex_detail::NowUnix() - seen < 180.0) online = true;
                if (online) m1.friendsOnline.insert(id);
            }
        }
        Parlex.GetFriendRequests([r, done](const ParlexResult& q) {
            auto& m2 = parlex_detail::Module();
            m2.friendsLoading = false;
            if (q.Ok) {
                std::set<std::string> before;
                for (const auto& p : m2.received) before.insert(p.first);
                m2.received.clear();
                m2.sent.clear();
                for (const Json& o : q.GetList("Incoming")) {
                    const std::string id = o["PlayFabId"].AsString();
                    if (!id.empty()) m2.received[id] = o["DisplayName"].AsString();
                }
                for (const Json& o : q.GetList("Outgoing")) {
                    const std::string id = o["PlayFabId"].AsString();
                    if (!id.empty()) m2.sent[id] = o["DisplayName"].AsString();
                }
                m2.friendsLoaded = r.Ok;
                if (m2.friendsLoaded) {
                    const auto received = m2.received;
                    for (const auto& p : received) {
                        if (!before.count(p.first)) parlex_detail::Fire(m2.friendsRequest, p.first, p.second);
                    }
                }
            }
            parlex_detail::Fire(m2.friendsChanged);
            if (done) done(q.Ok ? r : q);
        });
    });
}

inline bool ParlexFriendsFacade::Due(float everySeconds) const {
    const auto& m = parlex_detail::Module();
    return Parlex.LoggedIn() && !m.friendsLoading && parlex_detail::NowUnix() - m.friendsLastLoad >= everySeconds;
}

inline void ParlexFriendsFacade::Request(const std::string& id, const std::string& name, std::function<void()> done) const {
    if (id.empty() || !Parlex.LoggedIn() || id == Parlex.PlayerId()) return;
    auto& m = parlex_detail::Module();
    m.sent[id] = name;
    parlex_detail::Fire(m.friendsChanged);
    Parlex.SendFriendRequest(id, [id, name, done](const ParlexResult& r) {
        auto& m1 = parlex_detail::Module();
        if (!r.Ok) {
            m1.sent.erase(id);
            parlex_detail::Fire(m1.friendsChanged);
        } else if (r.GetString("Status") == "friends") {
            m1.sent.erase(id);
            m1.received.erase(id);
            m1.friends[id] = name;
            parlex_detail::Fire(m1.friendsChanged);
        }
        if (done) done();
    });
}

inline void ParlexFriendsFacade::Accept(const std::string& id, std::function<void()> done) const {
    if (id.empty() || !Parlex.LoggedIn()) return;
    auto& m = parlex_detail::Module();
    const auto it = m.received.find(id);
    const bool hadName = it != m.received.end();
    const std::string name = hadName ? it->second : std::string();
    m.received.erase(id);
    m.friends[id] = name;
    parlex_detail::Fire(m.friendsChanged);
    Parlex.AcceptFriendRequest(id, [id, name, hadName, done](const ParlexResult& r) {
        if (!r.Ok) {
            auto& m1 = parlex_detail::Module();
            m1.friends.erase(id);
            if (hadName) m1.received[id] = name;
            parlex_detail::Fire(m1.friendsChanged);
        }
        if (done) done();
    });
}

inline void ParlexFriendsFacade::Decline(const std::string& id, std::function<void()> done) const {
    if (id.empty() || !Parlex.LoggedIn()) return;
    auto& m = parlex_detail::Module();
    m.received.erase(id);
    parlex_detail::Fire(m.friendsChanged);
    Parlex.DeclineFriendRequest(id, [done](const ParlexResult&) { if (done) done(); });
}

inline void ParlexFriendsFacade::Cancel(const std::string& id, std::function<void()> done) const {
    if (id.empty() || !Parlex.LoggedIn()) return;
    auto& m = parlex_detail::Module();
    m.sent.erase(id);
    parlex_detail::Fire(m.friendsChanged);
    Parlex.CancelFriendRequest(id, [done](const ParlexResult&) { if (done) done(); });
}

inline void ParlexFriendsFacade::Remove(const std::string& id, std::function<void()> done) const {
    if (id.empty() || !Parlex.LoggedIn()) return;
    auto& m = parlex_detail::Module();
    m.friends.erase(id);
    parlex_detail::Fire(m.friendsChanged);
    Parlex.RemoveFriend(id, [done](const ParlexResult&) {
        ParlexFriends.Refresh();
        if (done) done();
    });
}

inline int ParlexFriendsFacade::OnChanged(ChangedFn fn) const {
    auto& m = parlex_detail::Module();
    const int h = m.nextHandle++;
    m.friendsChanged[h] = std::move(fn);
    return h;
}
inline int ParlexFriendsFacade::OnRequestArrived(RequestFn fn) const {
    auto& m = parlex_detail::Module();
    const int h = m.nextHandle++;
    m.friendsRequest[h] = std::move(fn);
    return h;
}
inline void ParlexFriendsFacade::Off(int handle) const {
    auto& m = parlex_detail::Module();
    m.friendsChanged.erase(handle);
    m.friendsRequest.erase(handle);
}

// ---------------------------------------------------------------------------
// ParlexSavedMutes
// ---------------------------------------------------------------------------
inline bool ParlexSavedMutesFacade::Loaded() const { return parlex_detail::Module().mutesLoaded; }
inline bool ParlexSavedMutesFacade::Contains(const std::string& id) const {
    return !id.empty() && parlex_detail::Module().mutes.count(id) != 0;
}
inline std::vector<std::string> ParlexSavedMutesFacade::All() const {
    const auto& s = parlex_detail::Module().mutes;
    return std::vector<std::string>(s.begin(), s.end());
}
inline void ParlexSavedMutesFacade::Load(std::function<void()> done) const {
    auto& m = parlex_detail::Module();
    if (m.mutesLoading || !Parlex.LoggedIn()) return;
    const std::string me = Parlex.PlayerId();
    if (m.mutesOwner != me) {
        m.mutes.clear();
        m.mutesLoaded = false;
        m.mutesOwner = me;
    }
    m.mutesLoading = true;
    Parlex.GetMyMutes([done](const ParlexResult& r) {
        auto& m1 = parlex_detail::Module();
        m1.mutesLoading = false;
        if (!r.Ok) return;
        m1.mutes.clear();
        for (const Json& o : r.GetList("Muted")) {
            const std::string id = o["PlayFabId"].AsString();
            if (!id.empty()) m1.mutes.insert(id);
        }
        m1.mutesLoaded = true;
        if (done) done();
    });
}
inline void ParlexSavedMutesFacade::Set(const std::string& id, bool on) const {
    if (id.empty() || !Parlex.LoggedIn()) return;
    auto& m = parlex_detail::Module();
    if (on) {
        m.mutes.insert(id);
        Parlex.AddMyMute(id, ParlexCallback());
    } else {
        m.mutes.erase(id);
        Parlex.RemoveMyMute(id, ParlexCallback());
    }
}

// ---------------------------------------------------------------------------
// ParlexIntegrity
// ---------------------------------------------------------------------------
inline bool ParlexIntegrityFacade::DebuggerAttached() const {
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.compare(0, 10, "TracerPid:") == 0) return std::atoi(line.c_str() + 10) != 0;
    }
#endif
    return false;
}

inline void ParlexIntegrityFacade::Report(ParlexCallback done) const {
    const std::string sig = SigningHash();
    const std::string version = Parlex.ClientVersion();
    Parlex.ReportIntegrity(sig, version, DebuggerAttached(), [done](const ParlexResult& r) {
        if (r.Ok && !r.GetBool("Ok")) {
            std::string reasons;
            for (const Json& reason : r.GetList("Reasons")) {
                if (!reasons.empty()) reasons += "; ";
                reasons += reason.AsString();
            }
            if (ScriptContext* c = ctxPtr()) {
                c->AddConsoleMessage("[Parlex] this build did not pass the server's check: " + reasons, ConsoleMessageType::Warning);
            }
        }
        if (done) done(r);
    });
}

// ---------------------------------------------------------------------------
// ParlexErrorReporter
// ---------------------------------------------------------------------------
inline void ParlexErrorReporterFacade::Report(const std::string& message, const std::string& stack) const {
    if (message.empty() || message.compare(0, 8, "[Parlex]") == 0) return;
    auto& m = parlex_detail::Module();
    const std::string key = message.size() > 100 ? message.substr(0, 100) : message;
    if (m.errorsSeen.size() >= 50 || !m.errorsSeen.insert(key).second) return;
    if (m.errorQueue.size() < 10) m.errorQueue.emplace_back(message, stack);
    Flush();
}

inline void ParlexErrorReporterFacade::Flush() const {
    auto& m = parlex_detail::Module();
    int sent = std::atoi(parlex_detail::Read("errors.sent", "0").c_str());
#if defined(_WIN32)
    const char* platform = "WindowsPlayer";
#elif defined(__ANDROID__)
    const char* platform = "Android";
#else
    const char* platform = "LinuxPlayer";
#endif
    while (Parlex.LoggedIn() && !m.errorQueue.empty() && sent < 10) {
        const auto entry = m.errorQueue.front();
        m.errorQueue.erase(m.errorQueue.begin());
        ++sent;
        parlex_detail::Write("errors.sent", std::to_string(sent));
        Parlex.Call("ReportClientError", Json::Map({{"Message", entry.first}, {"Stack", entry.second},
                                                    {"Version", Parlex.ClientVersion()}, {"Platform", platform}}),
                    ParlexCallback());
    }
}

// ---------------------------------------------------------------------------
// ParlexServerAgent
// ---------------------------------------------------------------------------
inline std::string ParlexServerAgentFacade::FindKey() const {
#if defined(__linux__)
    std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
    std::vector<std::string> args;
    std::string arg;
    while (std::getline(cmdline, arg, '\0')) args.push_back(arg);
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "-parlexKey") return args[i + 1];
    }
#endif
    const char* env = std::getenv("PARLEX_KEY");
    return env ? parlex_detail::Trim(env) : std::string();
}

inline void ParlexServerAgentFacade::Heartbeat(const std::string& serverId, int players, int maxPlayers,
                                               ParlexCallback done) const {
    const std::string key = FindKey();
    if (key.empty()) {
        if (done) done(ParlexResult::Failure("NoServerKey", "Start the server with -parlexKey <key> or set PARLEX_KEY."));
        return;
    }
    if (serverId.empty()) {
        if (done) done(ParlexResult::Failure("NoServerId", "Set the server id for the heartbeat."));
        return;
    }
    const Json body = Json::Map({{"ServerId", serverId}, {"Players", players}, {"MaxPlayers", maxPlayers},
                                 {"Version", Parlex.ClientVersion()}});
    Parlex.Send("/Server/ServerHeartbeat", body, false, [done](const ParlexResult& r) {
        if (r.Ok) parlex_detail::Write("agent.command", r.GetString("Command", "run"));
        if (done) done(r);
    }, HttpHeaders{{"X-Parlex-Key", key}}, 15);
}

// ---------------------------------------------------------------------------
// Notifications, moderator check, announcements (additions over the Unity SDK)
// ---------------------------------------------------------------------------
namespace parlex_detail {

// Per-user data folder for Parlex bookkeeping (last seen announcement / version).
inline std::string DataFile(const std::string& name) {
    std::string base;
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    base = appdata ? std::string(appdata) + "/Modularity/parlex" : std::string();
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    if (xdg && *xdg) base = std::string(xdg) + "/Modularity/parlex";
    else if (home && *home) base = std::string(home) + "/.local/share/Modularity/parlex";
#endif
    if (base.empty()) return std::string();
    return base + "/" + name;
}
inline std::string ReadFile(const std::string& path) {
    if (path.empty()) return std::string();
    std::ifstream f(path);
    std::string line;
    if (f) std::getline(f, line);
    return Trim(line);
}
inline void WriteFile(const std::string& path, const std::string& value) {
    if (path.empty()) return;
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        std::string dir = path.substr(0, slash);
        // Create each missing level; ignore failures (the next write just fails too).
        for (size_t i = 1; i <= dir.size(); ++i) {
            if (i == dir.size() || dir[i] == '/') {
                const std::string part = dir.substr(0, i);
#if defined(_WIN32)
                std::string cmd = "mkdir \"" + part + "\" >nul 2>&1";
#else
                std::string cmd = "mkdir -p '" + part + "' >/dev/null 2>&1";
#endif
                if (i == dir.size()) std::system(cmd.c_str());
            }
        }
    }
    std::ofstream f(path, std::ios::trunc);
    if (f) f << value << "\n";
}

}  // namespace parlex_detail

// ParlexNotify - cross-script popup queue. Any script pushes; ParlexNotifications.moducpp
// (or your own UI) pops and displays. Kinds: "info", "success", "friend", "announcement",
// "update", "warning".
struct ParlexNotifyFacade {
    void Push(const std::string& title, const std::string& body = std::string(), const std::string& kind = "info",
              float seconds = 5.0f) const {
        Json queue = Json::Parse(parlex_detail::Read("notify.queue"));
        if (!queue.IsArray()) queue = Json::Array();
        if (queue.Size() >= 20) return;
        queue.Push(Json::Map({{"Title", title}, {"Body", body}, {"Kind", kind}, {"Seconds", seconds}}));
        parlex_detail::Write("notify.queue", queue.Dump());
    }
    bool Pop(Json& out) const {
        Json queue = Json::Parse(parlex_detail::Read("notify.queue"));
        if (!queue.IsArray() || queue.Empty()) return false;
        const std::vector<Json>& items = queue.Items();
        out = items[0];
        Json rest = Json::Array();
        for (size_t i = 1; i < items.size(); ++i) rest.Push(items[i]);
        parlex_detail::Write("notify.queue", rest.Dump());
        return true;
    }
    int Count() const { return static_cast<int>(Json::Parse(parlex_detail::Read("notify.queue")).Size()); }
};

// ParlexModerator - true when the signed-in player owns one of the moderator items.
// Item ids are a comma-separated list (default "moderator"); the ids found in the
// player's inventory are kept in SeenItemIds() so you can see what to configure.
struct ParlexModeratorFacade {
    void SetItemIds(const std::string& csv) const { parlex_detail::Write("mod.items", csv); }
    std::string ItemIds() const { return parlex_detail::Read("mod.items", "moderator"); }
    bool IsModerator() const { return parlex_detail::Read("mod.active") == "1"; }
    std::string SeenItemIds() const { return parlex_detail::Read("mod.seen"); }
    unsigned int Revision() const { return parlex_detail::Revision("mod.active"); }
    void Refresh(ParlexCallback done = ParlexCallback()) const {
        Parlex.GetInventory([done](const ParlexResult& r) {
            if (r.Ok) {
                std::set<std::string> seen;
                for (const char* path : {"Inventory", "Items"}) {
                    for (const Json& item : r.GetList(path)) {
                        std::string id = item["ItemId"].AsString();
                        if (id.empty()) id = item["Id"].AsString();
                        if (!id.empty()) seen.insert(id);
                    }
                }
                std::string seenCsv, wanted = parlex_detail::Read("mod.items", "moderator");
                bool found = false;
                std::set<std::string> wantSet;
                size_t start = 0;
                while (start <= wanted.size()) {
                    const size_t comma = wanted.find(',', start);
                    const std::string part = parlex_detail::Lower(parlex_detail::Trim(
                        wanted.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
                    if (!part.empty()) wantSet.insert(part);
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
                for (const auto& id : seen) {
                    if (!seenCsv.empty()) seenCsv += ",";
                    seenCsv += id;
                    if (wantSet.count(parlex_detail::Lower(id))) found = true;
                }
                parlex_detail::Write("mod.seen", seenCsv);
                if (found != (parlex_detail::Read("mod.active") == "1") || !parlex_detail::Exists("mod.active")) {
                    parlex_detail::Write("mod.active", found ? "1" : "");
                }
            }
            if (done) done(r);
        });
    }
};

// ParlexAnnouncements - the console's active announcement (Schedule > Show an announcement).
struct ParlexAnnouncementsFacade {
    std::string Text() const { return ParlexServerGate.Current().AnnouncementText; }
    std::string From() const { return ParlexServerGate.Current().AnnouncementFrom; }
    std::string Id() const { return ParlexServerGate.Current().AnnouncementId; }
    bool Active() const { return !Text().empty(); }
    std::string SeenFile() const { return parlex_detail::DataFile(Parlex.TitleId() + ".announcement"); }
    // True when this announcement has not been shown on this machine yet.
    bool IsNew() const {
        const std::string id = Id().empty() ? Text() : Id();
        return !Text().empty() && parlex_detail::ReadFile(SeenFile()) != id;
    }
    void MarkSeen() const {
        parlex_detail::WriteFile(SeenFile(), Id().empty() ? Text() : Id());
    }
    // The last email used to sign in on this machine (never the password).
    std::string RememberedEmail() const { return parlex_detail::ReadFile(parlex_detail::DataFile(Parlex.TitleId() + ".email")); }
    void RememberEmail(const std::string& email) const { parlex_detail::WriteFile(parlex_detail::DataFile(Parlex.TitleId() + ".email"), email); }
    // Returns the previously played version when the game was updated since last run
    // ("" when first run or unchanged), and records the current version.
    std::string CheckUpdated() const {
        const std::string file = parlex_detail::DataFile(Parlex.TitleId() + ".version");
        const std::string previous = parlex_detail::ReadFile(file);
        const std::string now = Parlex.ClientVersion();
        if (previous != now) parlex_detail::WriteFile(file, now);
        return previous == now ? std::string() : previous;
    }
};

inline const ParlexNotifyFacade ParlexNotify{};
inline const ParlexModeratorFacade ParlexModerator{};
inline const ParlexAnnouncementsFacade ParlexAnnouncements{};

}  // namespace ModuCPP
