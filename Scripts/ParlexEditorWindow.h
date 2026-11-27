#pragma once
// Parlex console window for the Modularity editor (port of Parlex for Unity's
// Window > CSVRStudios > Parlex). Tabs: Connect, Overview, Players, Servers, Errors,
// Data, Moderator. Every call goes to {host}/t/{titleId}/Server/* with the Title
// secret in X-Parlex-Key. The secret is stored in your user data folder
// (~/.local/share/Modularity/parlex/<title>.secret, owner-only), never in the project
// and never in a build.

#include "ParlexScriptApi.h"
#include "imgui.h"

#include <filesystem>
#include <functional>
#include <map>
#include <regex>
#include <string>

namespace ParlexEditor {

using namespace ModuCPP;

struct Entry {
    Json data;
    bool has = false, ok = false, loading = false;
    std::string error;
    double requested = 0.0;
};

struct State {
    std::string host = "https://parlex.csvrstudios.com";
    std::string titleId, secret;
    bool connected = false, loaded = false;
    std::string status;
    bool statusBad = false;
    std::string playerSearch, openPlayer, confirm, notice;
    std::string banReason = "Banned from the Parlex window";
    int banHours = 24;
    std::string modTarget;
    std::map<std::string, Entry> cache;
    std::string editKey, editValue, newKey, newValue;
};

inline State& St() {
    static State state;
    return state;
}

inline std::string SecretFile(const std::string& title) {
    return parlex_detail::DataFile(title + ".secret");
}

inline bool Ready() {
    const State& s = St();
    return !s.titleId.empty() && !s.secret.empty();
}

inline std::string Base() {
    State& s = St();
    std::string host = s.host;
    while (!host.empty() && host.back() == '/') host.pop_back();
    return host + "/t/" + s.titleId;
}

inline void Post(const std::string& path, const Json& body, std::function<void(const ParlexResult&)> done) {
    State& s = St();
    HttpHeaders headers{{"X-Parlex-Key", s.secret}};
    Http.PostJson(Base() + path, body, [done](const HttpResponse& r) {
        std::string netError = r.Error;
        if (netError.empty() && (r.Status < 200 || r.Status >= 300)) netError = "HTTP error " + std::to_string(r.Status);
        if (done) done(ParlexResult::FromResponse(r.Status, r.Body, netError));
    }, headers, 20);
}

inline void Fetch(const std::string& key, const std::string& path, const Json& body = Json::Object()) {
    Entry& e = St().cache[key];
    const double now = parlex_detail::NowUnix();
    if (e.loading && now - e.requested > 25.0) e.loading = false;  // callback dropped (play toggled)
    if (e.has || e.loading) return;
    e.loading = true;
    e.requested = now;
    Post(path, body, [key](const ParlexResult& r) {
        Entry& entry = St().cache[key];
        entry.loading = false;
        entry.has = true;
        entry.ok = r.Ok;
        entry.data = r.Data;
        entry.error = r.Ok ? std::string() : (r.ErrorName == "NotAuthorized" ? std::string("That Title ID and secret do not match.") : r.Error);
    });
}

inline void Drop(const std::string& key) { St().cache.erase(key); }
inline void DropAll() { St().cache.clear(); }

// Returns the entry when it has loaded; draws a loading / error line otherwise.
inline const Entry* Got(const std::string& key) {
    const auto it = St().cache.find(key);
    if (it == St().cache.end() || !it->second.has) {
        ImGui::TextDisabled("Loading...");
        return nullptr;
    }
    if (!it->second.ok) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.45f, 1.0f), "%s", it->second.error.c_str());
        if (ImGui::SmallButton("Try again")) Drop(key);
        return nullptr;
    }
    return &it->second;
}

inline bool InputStr(const char* label, std::string& value, size_t capacity = 256, int flags = 0) {
    std::string buffer = value;
    buffer.resize(capacity);
    if (ImGui::InputText(label, &buffer[0], capacity, flags)) {
        value = buffer.c_str();
        return true;
    }
    return false;
}

inline void Tree(const std::string& label, const Json& v, int depth = 0) {
    if (v.IsObject() || v.IsArray()) {
        if (depth > 8) return;
        const std::string title = label + (v.IsArray() ? " [" + std::to_string(v.Size()) + "]" : "");
        if (ImGui::TreeNode(title.c_str())) {
            if (v.IsObject()) {
                for (size_t i = 0; i < v.Keys().size(); ++i) Tree(v.Keys()[i], v.Values()[i], depth + 1);
            } else {
                for (size_t i = 0; i < v.Size(); ++i) Tree(std::to_string(i), v.Items()[i], depth + 1);
            }
            ImGui::TreePop();
        }
        return;
    }
    ImGui::TextWrapped("%s: %s", label.c_str(), v.IsNull() ? "-" : v.AsString().c_str());
}

inline void Notice() {
    State& s = St();
    if (s.notice.empty()) return;
    ImGui::TextColored(ImVec4(0.45f, 0.85f, 1.0f, 1.0f), "%s", s.notice.c_str());
}

inline void Connect() {
    State& s = St();
    s.titleId.erase(0, s.titleId.find_first_not_of(" \t"));
    while (!s.titleId.empty() && (s.titleId.back() == ' ' || s.titleId.back() == '\t')) s.titleId.pop_back();
    if (s.titleId.empty() || s.secret.empty()) { s.status = "Enter your Title ID and the Title secret."; s.statusBad = true; return; }
    if (s.host.compare(0, 8, "https://") != 0) { s.status = "The Parlex address must start with https://"; s.statusBad = true; return; }
    s.status = "Connecting...";
    s.statusBad = false;
    DropAll();
    Post("/Server/GetOverview", Json::Object(), [](const ParlexResult& r) {
        State& st = St();
        if (!r.Ok) {
            st.connected = false;
            st.statusBad = true;
            st.status = r.ErrorName == "NotAuthorized" ? "That Title ID and secret do not match. Check them in the console under Settings."
                                                       : "Could not connect: " + r.Error;
            return;
        }
        st.connected = true;
        st.statusBad = false;
        parlex_detail::WriteFile(SecretFile(st.titleId), st.secret);
#if !defined(_WIN32)
        std::error_code ec;
        std::filesystem::permissions(SecretFile(st.titleId), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ec);
#endif
        parlex_detail::WriteFile(parlex_detail::DataFile("window.title"), st.titleId);
        st.status = "Connected. This game has " + std::to_string(r.GetInt("Players")) + " players.";
    });
}

inline void DrawConnect() {
    State& s = St();
    InputStr("Parlex address", s.host);
    InputStr("Title ID", s.titleId, 64);
    InputStr("Title secret", s.secret, 256, ImGuiInputTextFlags_Password);
    ImGui::TextDisabled("Both are on the Settings page of your game in the console. The secret stays on this computer.");
    if (ImGui::Button("Save and connect")) Connect();
    ImGui::SameLine();
    if (ImGui::Button("Forget secret")) {
        if (!s.titleId.empty()) parlex_detail::WriteFile(SecretFile(s.titleId), "");
        s.secret.clear();
        s.connected = false;
        DropAll();
        s.status = "Secret forgotten.";
        s.statusBad = false;
    }
    if (!s.status.empty()) {
        ImGui::TextColored(s.statusBad ? ImVec4(1.0f, 0.5f, 0.45f, 1.0f) : ImVec4(0.55f, 1.0f, 0.65f, 1.0f), "%s", s.status.c_str());
    }
}

inline void NeedConnect(const char* what) {
    ImGui::TextWrapped("Enter your Title ID and secret on the Connect tab to %s.", what);
}

inline void DrawOverview() {
    State& s = St();
    if (!Ready()) { NeedConnect("see your game's numbers"); return; }
    Fetch("overview", "/Server/GetOverview");
    Fetch("policy", "/Server/GetPolicy");
    if (const Entry* e = Got("overview")) Tree("Overview", e->data);
    ImGui::Separator();
    if (const Entry* p = Got("policy")) {
        const bool loginOn = p->data["LoginEnabled"].AsBool(true);
        ImGui::Text("Game is %s", loginOn ? "OPEN" : "CLOSED (maintenance)");
        static std::string message;
        InputStr("Maintenance message", message, 200);
        if (ImGui::Button(loginOn ? "Close the game" : "Open the game")) {
            Post("/Server/SetPolicy", Json::Map({{"AllowNewAccounts", p->data["AllowNewAccounts"].AsBool(true)},
                                                 {"LoginEnabled", !loginOn},
                                                 {"MaintenanceMessage", loginOn ? message : std::string()},
                                                 {"ClientFriends", p->data["ClientFriends"].AsBool(true)}}),
                 [](const ParlexResult&) { Drop("policy"); });
        }
    }
    ImGui::Separator();
    if (ImGui::Button("Refresh")) { Drop("overview"); Drop("policy"); Drop("analytics"); }
    ImGui::SameLine();
    if (ImGui::Button("Load analytics")) Fetch("analytics", "/Server/GetAnalytics");
    if (St().cache.count("analytics")) {
        if (const Entry* a = Got("analytics")) Tree("Analytics", a->data);
    }
    (void)s;
}

inline void DrawPlayers() {
    State& s = St();
    if (!Ready()) { NeedConnect("look up players"); return; }
    InputStr("Search", s.playerSearch, 120);
    ImGui::SameLine();
    if (ImGui::Button("Search")) { Drop("players"); s.openPlayer.clear(); }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) { s.playerSearch.clear(); Drop("players"); s.openPlayer.clear(); }
    Fetch("players", "/Server/ListPlayers", Json::Map({{"Search", s.playerSearch}, {"Limit", 25}}));
    const Entry* res = Got("players");
    if (!res) return;
    ImGui::TextDisabled("%d players", res->data["Total"].AsInt());
    for (const Json& p : res->data["Players"].Items()) {
        const std::string id = p["PlayFabId"].AsString();
        const std::string name = p["DisplayName"].AsString();
        const bool banned = p["Banned"].AsBool(false);
        ImGui::PushID(id.c_str());
        const std::string label = (name.empty() ? std::string("(no name)") : name) + (banned ? "  [banned]" : "") + "###p";
        const bool open = ImGui::CollapsingHeader(label.c_str());
        if (open) {
            const std::string key = "prof:" + id;
            Fetch(key, "/Server/GetPlayerProfileFull", Json::Map({{"PlayFabId", id}}));
            ImGui::TextDisabled("%s   %s", id.c_str(), p["Country"].AsString().c_str());
            if (const Entry* prof = Got(key)) {
                Tree("Profile", prof->data);
                const bool bannedNow = prof->data["Banned"].AsBool(banned);
                InputStr("Reason", s.banReason, 160);
                ImGui::SetNextItemWidth(120);
                ImGui::InputInt("Hours (0 = permanent)", &s.banHours);
                if (s.banHours < 0) s.banHours = 0;
                if (!bannedNow) {
                    if (s.confirm == "ban:" + id) {
                        if (ImGui::Button("Really ban?")) {
                            Json ban = Json::Map({{"PlayFabId", id}, {"Reason", s.banReason}});
                            if (s.banHours > 0) ban["DurationInHours"] = s.banHours;
                            Post("/Server/BanUsers", Json::Map({{"Bans", Json::List({ban})}}),
                                 [id](const ParlexResult&) { Drop("players"); Drop("prof:" + id); });
                            s.confirm.clear();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel")) s.confirm.clear();
                    } else if (ImGui::Button("Ban")) {
                        s.confirm = "ban:" + id;
                    }
                } else if (ImGui::Button("Unban")) {
                    Post("/Server/RevokeAllBansForUser", Json::Map({{"PlayFabId", id}}),
                         [id](const ParlexResult&) { Drop("players"); Drop("prof:" + id); });
                }
            }
        }
        ImGui::PopID();
    }
}

inline void DrawServers() {
    if (!Ready()) { NeedConnect("manage servers"); return; }
    Fetch("servers", "/Server/ListServers");
    if (ImGui::Button("Refresh")) Drop("servers");
    const Entry* res = Got("servers");
    if (!res) return;
    static const char* kStates[] = {"online", "maintenance", "offline"};
    const Json& list = res->data["Servers"].IsArray() ? res->data["Servers"] : res->data["Items"];
    if (list.Empty()) ImGui::TextDisabled("No servers listed yet.");
    for (const Json& sv : list.Items()) {
        const std::string id = sv["ServerId"].AsString();
        ImGui::PushID(id.c_str());
        const int ccu = sv["Ccu"].AsInt(), limit = sv["CcuLimit"].AsInt();
        ImGui::Text("%s  (%s %s)  %d%s", sv["Name"].AsString().c_str(), sv["Product"].AsString().c_str(),
                    sv["Region"].AsString().c_str(), ccu, limit > 0 ? (" / " + std::to_string(limit)).c_str() : "");
        int current = 0;
        for (int i = 0; i < 3; ++i) if (sv["Status"].AsString() == kStates[i]) current = i;
        ImGui::SetNextItemWidth(150);
        int pick = current;
        if (ImGui::Combo("Status", &pick, kStates, 3) && pick != current) {
            Post("/Server/SetServerStatus", Json::Map({{"ServerId", id}, {"Status", std::string(kStates[pick])}}),
                 [](const ParlexResult&) { Drop("servers"); });
        }
        ImGui::PopID();
        ImGui::Separator();
    }
}

inline void DrawErrors() {
    State& s = St();
    if (!Ready()) { NeedConnect("see the errors your players hit"); return; }
    Fetch("errors", "/Server/ListClientErrors");
    if (ImGui::Button("Refresh")) Drop("errors");
    ImGui::SameLine();
    if (ImGui::Button("Send test error")) {
        Post("/Server/SendTestError", Json::Map({{"Version", std::string(Parlex.ClientVersion())}}), [](const ParlexResult& r) {
            St().notice = r.Ok ? "Test error sent. It is listed below." : "Could not send it: " + r.Error;
            Drop("errors");
        });
    }
    ImGui::SameLine();
    if (s.confirm == "clearErrors") {
        if (ImGui::Button("Really clear all?")) {
            Post("/Server/ClearClientErrors", Json::Object(), [](const ParlexResult&) { Drop("errors"); });
            s.confirm.clear();
        }
    } else if (ImGui::Button("Clear all")) {
        s.confirm = "clearErrors";
    }
    Notice();
    const Entry* res = Got("errors");
    if (!res) return;
    ImGui::TextDisabled("%d different problems, %d reports in 24 hours", res->data["Total"].AsInt(), res->data["Last24h"].AsInt());
    for (const Json& e : res->data["Errors"].Items()) {
        const std::string id = e["Id"].AsString();
        ImGui::PushID(id.c_str());
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.45f, 1.0f), "%sx", e["Count"].AsString().c_str());
        ImGui::SameLine();
        ImGui::TextWrapped("%s", e["Message"].AsString().c_str());
        ImGui::TextDisabled("version %s  %s  ~%s players", e["Version"].AsString().c_str(), e["Platform"].AsString().c_str(),
                            e["Players"].AsString().c_str());
        if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText((e["Message"].AsString() + "\n" + e["Stack"].AsString()).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Done")) {
            Post("/Server/ClearClientErrors", Json::Map({{"Id", id}}), [](const ParlexResult&) { Drop("errors"); });
        }
        ImGui::PopID();
        ImGui::Separator();
    }
}

inline void DrawData() {
    State& s = St();
    if (!Ready()) { NeedConnect("edit your title data"); return; }
    Fetch("tdata", "/Server/GetTitleInternalData");
    if (ImGui::Button("Refresh")) Drop("tdata");
    Notice();
    const Entry* res = Got("tdata");
    if (!res) return;
    const Json& data = res->data["Data"].IsObject() ? res->data["Data"] : res->data;
    for (size_t i = 0; i < data.Keys().size(); ++i) {
        const std::string key = data.Keys()[i];
        ImGui::PushID(key.c_str());
        const bool hidden = std::regex_search(key, std::regex("secret|token|password|key", std::regex::icase));
        ImGui::Text("%s", key.c_str());
        if (s.editKey == key) {
            InputStr("##value", s.editValue, 2000);
            if (ImGui::SmallButton("Save")) {
                Post("/Server/SetTitleInternalData", Json::Map({{"Key", key}, {"Value", s.editValue}}), [](const ParlexResult&) {
                    St().editKey.clear();
                    Drop("tdata");
                });
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel")) s.editKey.clear();
        } else {
            ImGui::TextDisabled("%s", hidden ? "(hidden)" : data.Values()[i].AsString().c_str());
            if (ImGui::SmallButton("Edit")) { s.editKey = key; s.editValue = hidden ? std::string() : data.Values()[i].AsString(); }
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) {
                Post("/Server/SetTitleInternalData", Json::Map({{"Key", key}, {"Value", Json()}}), [](const ParlexResult&) { Drop("tdata"); });
            }
        }
        ImGui::PopID();
        ImGui::Separator();
    }
    ImGui::TextUnformatted("Add a value");
    InputStr("Key", s.newKey, 100);
    InputStr("Value", s.newValue, 2000);
    if (ImGui::Button("Add") && !s.newKey.empty()) {
        const std::string key = s.newKey;
        Post("/Server/SetTitleInternalData", Json::Map({{"Key", key}, {"Value", s.newValue}}), [key](const ParlexResult& r) {
            St().notice = r.Ok ? "Added " + key + "." : "Could not add it: " + r.Error;
            St().newKey.clear();
            St().newValue.clear();
            Drop("tdata");
        });
    }
}

inline void DrawModerator() {
    State& s = St();
    const bool signedIn = Parlex.LoggedIn();
    ImGui::TextWrapped("Moderator tools unlock for a signed-in player who owns a moderator item. Items are matched by id (comma-separated); sign in during Play to check.");
    static std::string items = ParlexModerator.ItemIds();
    if (InputStr("Moderator item ids", items, 200)) ParlexModerator.SetItemIds(items);
    if (signedIn && ImGui::Button("Check my inventory")) ParlexModerator.Refresh();
    ImGui::Text("Signed in: %s    Moderator: %s", signedIn ? "yes" : "no", ParlexModerator.IsModerator() ? "YES" : "no");
    const std::string seen = ParlexModerator.SeenItemIds();
    ImGui::TextDisabled("Items in this player's inventory: %s", seen.empty() ? "(none or not checked)" : seen.c_str());
    ImGui::Separator();
    if (!ParlexModerator.IsModerator()) { ImGui::TextDisabled("Quick actions are hidden until a moderator item is detected."); return; }
    if (!Ready()) { NeedConnect("use quick actions"); return; }
    InputStr("Player id", s.modTarget, 64);
    InputStr("Reason", s.banReason, 160);
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Hours (0 = permanent)", &s.banHours);
    if (s.banHours < 0) s.banHours = 0;
    if (ImGui::Button("Ban player") && !s.modTarget.empty()) {
        Json ban = Json::Map({{"PlayFabId", s.modTarget}, {"Reason", s.banReason}});
        if (s.banHours > 0) ban["DurationInHours"] = s.banHours;
        Post("/Server/BanUsers", Json::Map({{"Bans", Json::List({ban})}}),
             [](const ParlexResult& r) { St().notice = r.Ok ? "Player banned." : "Could not ban: " + r.Error; });
    }
    ImGui::SameLine();
    if (ImGui::Button("Unban player") && !s.modTarget.empty()) {
        Post("/Server/RevokeAllBansForUser", Json::Map({{"PlayFabId", s.modTarget}}),
             [](const ParlexResult& r) { St().notice = r.Ok ? "Player unbanned." : "Could not unban: " + r.Error; });
    }
    Notice();
}

inline void Draw() {
    State& s = St();
    if (!s.loaded) {
        s.loaded = true;
        s.titleId = parlex_detail::ReadFile(parlex_detail::DataFile("window.title"));
        if (!s.titleId.empty()) s.secret = parlex_detail::ReadFile(SecretFile(s.titleId));
        if (Ready()) Connect();
    }
    ImGui::TextDisabled("Parlex by CSVRStudios - %s", s.connected ? "connected" : "not connected");
    if (!ImGui::BeginTabBar("##parlexTabs")) return;
    if (ImGui::BeginTabItem("Connect")) { DrawConnect(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Overview")) { DrawOverview(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Players")) { DrawPlayers(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Servers")) { DrawServers(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Errors")) { DrawErrors(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Data")) { DrawData(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Moderator")) { DrawModerator(); ImGui::EndTabItem(); }
    ImGui::EndTabBar();
}

}  // namespace ParlexEditor
