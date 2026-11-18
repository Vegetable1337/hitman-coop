#include "SteamCoop.h"
#include "coop/Protocol.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <charconv>
#include <cstring>
#include <string_view>
#include <utility>

namespace {
using Id = uint64_t;
constexpr int c_JoinRequested = 333;
constexpr int c_LobbyEntered = 504;
constexpr int c_LobbyCreated = 513;
constexpr int c_PrivateLobby = 0;
constexpr int c_Ok = 1;
constexpr int c_Channel = 0x48434f50;
constexpr int c_UnreliableNoNagle = 1;
constexpr char c_LobbyKey[] = "hitman_coop_transport";
constexpr char c_LobbyVersion[] = "1";

// Windows x64 ABI for the few Steamworks data structures used below. We resolve the
// versioned flat API from the game's existing DLL, with no second Steam context.
struct alignas(8) Identity { std::byte storage[136] {}; };
struct Message { const void* data; int size; uint32_t connection; Identity peer; };
struct Created { int result; Id lobby; };
struct Entered { Id lobby; uint32_t permissions; bool locked; uint32_t result; };
struct JoinRequested { Id lobby; Id friendId; };
static_assert(sizeof(Created) == 16 && sizeof(Entered) == 24 && sizeof(JoinRequested) == 16);
static_assert(offsetof(Message, peer) == 16);

// CCallbackBase ABI. Do not register a manual dispatcher or run Steam's callbacks here.
class Callback {
public:
    explicit Callback(int p_Kind, int p_Size) : m_Callback(p_Kind), m_Size(p_Size) {}
    virtual void Run(void* p_Data) { Run(p_Data, false, 0); }
    virtual void Run(void* p_Data, bool p_Failed, Id) {
        Id s_Lobby = 0;
        int s_Result = p_Failed ? -2 : 0;
        if (!p_Failed && p_Data) {
            if (m_Callback == c_LobbyCreated) {
                const auto& s_Data = *static_cast<const Created*>(p_Data);
                s_Lobby = s_Data.lobby;
                s_Result = s_Data.result;
            }
            else if (m_Callback == c_LobbyEntered) {
                const auto& s_Data = *static_cast<const Entered*>(p_Data);
                s_Lobby = s_Data.lobby;
                s_Result = static_cast<int>(s_Data.result);
            }
            else {
                s_Lobby = static_cast<const JoinRequested*>(p_Data)->lobby;
                s_Result = c_Ok;
            }
        }
        lobby.store(s_Lobby, std::memory_order_relaxed);
        result.store(s_Result, std::memory_order_release);
    }
    virtual int GetCallbackSizeBytes() { return m_Size; }

private:
    uint8_t m_Flags = 0; // Steam updates these two fields.
    int m_Callback; // Call results require the expected callback ID before registration.
    const int m_Size;
public:
    std::atomic<Id> lobby { 0 };
    std::atomic<int> result { -1 };
};

struct Api {
    void* matchmaking = nullptr;
    void* friends = nullptr;
    void* networking = nullptr;
    void* user = nullptr;
    void* utils = nullptr;
    void* apps = nullptr;

    Id (*create)(void*, int, int) = nullptr;
    Id (*join)(void*, Id) = nullptr;
    void (*leave)(void*, Id) = nullptr;
    Id (*owner)(void*, Id) = nullptr;
    int (*memberCount)(void*, Id) = nullptr;
    Id (*member)(void*, Id, int) = nullptr;
    bool (*setData)(void*, Id, const char*, const char*) = nullptr;
    const char* (*getData)(void*, Id, const char*) = nullptr;
    void (*invite)(void*, Id) = nullptr;
    bool (*overlayEnabled)(void*) = nullptr;
    uint32_t (*appId)(void*) = nullptr;
    int (*callFailure)(void*, Id) = nullptr;
    bool (*loggedOn)(void*) = nullptr;
    Id (*steamId)(void*) = nullptr;
    int (*launchLine)(void*, char*, int) = nullptr;
    void (*registerCallback)(Callback*, int) = nullptr;
    void (*unregisterCallback)(Callback*) = nullptr;
    void (*registerResult)(Callback*, Id) = nullptr;
    void (*unregisterResult)(Callback*, Id) = nullptr;
    void (*setId)(Identity*, Id) = nullptr;
    Id (*getId)(const Identity*) = nullptr;
    int (*send)(void*, const Identity*, const void*, uint32_t, int, int) = nullptr;
    int (*receive)(void*, int, Message**, int) = nullptr;
    bool (*accept)(void*, const Identity*) = nullptr;
    bool (*close)(void*, const Identity*, int) = nullptr;
    void (*release)(Message*) = nullptr;

    bool Load() {
        const auto s_Module = GetModuleHandleW(L"steam_api64.dll");
        if (!s_Module)
            return false;
        using Interface = void* (*)();
        const auto s_Interface = [&](const char* p_Name) {
            const auto s_Get = reinterpret_cast<Interface>(GetProcAddress(s_Module, p_Name));
            return s_Get ? s_Get() : nullptr;
        };
        matchmaking = s_Interface("SteamAPI_SteamMatchmaking_v009");
        friends = s_Interface("SteamAPI_SteamFriends_v018");
        networking = s_Interface("SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
        user = s_Interface("SteamAPI_SteamUser_v023");
        utils = s_Interface("SteamAPI_SteamUtils_v011");
        apps = s_Interface("SteamAPI_SteamApps_v009");
        if (!matchmaking || !friends || !networking || !user || !utils || !apps)
            return false;

#define COOP_STEAM_BIND(field, entry) \
        field = reinterpret_cast<decltype(field)>(GetProcAddress(s_Module, "SteamAPI_" #entry)); \
        if (!field) return false
        COOP_STEAM_BIND(create, ISteamMatchmaking_CreateLobby);
        COOP_STEAM_BIND(join, ISteamMatchmaking_JoinLobby);
        COOP_STEAM_BIND(leave, ISteamMatchmaking_LeaveLobby);
        COOP_STEAM_BIND(owner, ISteamMatchmaking_GetLobbyOwner);
        COOP_STEAM_BIND(memberCount, ISteamMatchmaking_GetNumLobbyMembers);
        COOP_STEAM_BIND(member, ISteamMatchmaking_GetLobbyMemberByIndex);
        COOP_STEAM_BIND(setData, ISteamMatchmaking_SetLobbyData);
        COOP_STEAM_BIND(getData, ISteamMatchmaking_GetLobbyData);
        COOP_STEAM_BIND(invite, ISteamFriends_ActivateGameOverlayInviteDialog);
        COOP_STEAM_BIND(overlayEnabled, ISteamUtils_IsOverlayEnabled);
        COOP_STEAM_BIND(appId, ISteamUtils_GetAppID);
        COOP_STEAM_BIND(callFailure, ISteamUtils_GetAPICallFailureReason);
        COOP_STEAM_BIND(loggedOn, ISteamUser_BLoggedOn);
        COOP_STEAM_BIND(steamId, ISteamUser_GetSteamID);
        COOP_STEAM_BIND(launchLine, ISteamApps_GetLaunchCommandLine);
        COOP_STEAM_BIND(registerCallback, RegisterCallback);
        COOP_STEAM_BIND(unregisterCallback, UnregisterCallback);
        COOP_STEAM_BIND(registerResult, RegisterCallResult);
        COOP_STEAM_BIND(unregisterResult, UnregisterCallResult);
        COOP_STEAM_BIND(setId, SteamNetworkingIdentity_SetSteamID64);
        COOP_STEAM_BIND(getId, SteamNetworkingIdentity_GetSteamID64);
        COOP_STEAM_BIND(send, ISteamNetworkingMessages_SendMessageToUser);
        COOP_STEAM_BIND(receive, ISteamNetworkingMessages_ReceiveMessagesOnChannel);
        COOP_STEAM_BIND(accept, ISteamNetworkingMessages_AcceptSessionWithUser);
        COOP_STEAM_BIND(close, ISteamNetworkingMessages_CloseChannelWithUser);
        COOP_STEAM_BIND(release, SteamNetworkingMessage_t_Release);
#undef COOP_STEAM_BIND
        return true;
    }
};

Id LaunchLobby(std::string_view p_Line) {
    constexpr std::string_view c_Option = "+connect_lobby ";
    const auto s_Pos = p_Line.find(c_Option);
    if (s_Pos == std::string_view::npos || (s_Pos != 0 && p_Line[s_Pos - 1] != ' '))
        return 0;
    p_Line.remove_prefix(s_Pos + c_Option.size());
    const auto s_End = p_Line.find_first_of(" \t");
    p_Line = p_Line.substr(0, s_End);
    Id s_Id = 0;
    const auto s_Parsed = std::from_chars(p_Line.data(), p_Line.data() + p_Line.size(), s_Id);
    return s_Parsed.ec == std::errc{} && s_Parsed.ptr == p_Line.data() + p_Line.size() ? s_Id : 0;
}

class SteamSocket final : public coop::IDatagramSocket {
public:
    SteamSocket(std::shared_ptr<Api> p_Api, Id p_Lobby, Id p_Local, Id p_Host, std::shared_ptr<Id> p_Hosted)
        : m_Api(std::move(p_Api)), m_Lobby(p_Lobby), m_Local(p_Local), m_Host(p_Host), m_Hosted(std::move(p_Hosted)) {}

    ~SteamSocket() override {
        SetPeer(0);
        m_Api->leave(m_Api->matchmaking, m_Lobby);
        if (*m_Hosted == m_Lobby)
            *m_Hosted = 0;
    }

    bool SendTo(const coop::Endpoint& p_To, const uint8_t* p_Data, size_t p_Size) override {
        if (!p_To.steamPeer || p_Size > coop::kMaxPacketSize)
            return false;
        if (p_To.steamPeer != m_Peer)
            RefreshPeer();
        if (p_To.steamPeer != m_Peer)
            return false;
        std::array<uint8_t, sizeof(Id) + coop::kMaxPacketSize> s_Data;
        std::memcpy(s_Data.data(), &m_Lobby, sizeof(Id));
        std::memcpy(s_Data.data() + sizeof(Id), p_Data, p_Size);
        return m_Api->send(m_Api->networking, &m_Identity, s_Data.data(),
            static_cast<uint32_t>(p_Size + sizeof(Id)), c_UnreliableNoNagle, c_Channel) == c_Ok;
    }

    int RecvFrom(coop::Endpoint& p_From, uint8_t* p_Buffer, size_t p_Capacity) override {
        RefreshPeer();
        // Steam only delivers after we accept the authenticated lobby peer.
        if (m_Peer && !m_Accepted)
            m_Accepted = m_Api->accept(m_Api->networking, &m_Identity);
        for (int i = 0; i < 16; ++i) {
            Message* s_Message = nullptr;
            if (m_Api->receive(m_Api->networking, c_Channel, &s_Message, 1) != 1)
                break;
            const std::unique_ptr<Message, decltype(Api::release)> s_Scoped(s_Message, m_Api->release);
            if (!s_Message || !m_Peer || m_Api->getId(&s_Message->peer) != m_Peer ||
                s_Message->size <= static_cast<int>(sizeof(Id)) || !s_Message->data)
                continue;
            const size_t s_Size = static_cast<size_t>(s_Message->size) - sizeof(Id);
            if (s_Size > p_Capacity || s_Size > coop::kMaxPacketSize)
                continue;
            Id s_Lobby = 0;
            std::memcpy(&s_Lobby, s_Message->data, sizeof(Id));
            if (s_Lobby != m_Lobby)
                continue;
            std::memcpy(p_Buffer, static_cast<const uint8_t*>(s_Message->data) + sizeof(Id), s_Size);
            p_From = coop::Endpoint::Steam(m_Peer);
            return static_cast<int>(s_Size);
        }
        return -1;
    }

private:
    void RefreshPeer() {
        Id s_Peer = 0;
        if (m_Api->owner(m_Api->matchmaking, m_Lobby) == m_Host &&
            m_Api->memberCount(m_Api->matchmaking, m_Lobby) == 2) {
            for (int i = 0; i < 2; ++i) {
                const Id s_Member = m_Api->member(m_Api->matchmaking, m_Lobby, i);
                if (s_Member != m_Local && (m_Host == m_Local || s_Member == m_Host))
                    s_Peer = s_Member;
            }
        }
        SetPeer(s_Peer);
    }

    void SetPeer(Id p_Peer) {
        if (m_Peer == p_Peer)
            return;
        if (m_Peer)
            m_Api->close(m_Api->networking, &m_Identity, c_Channel);
        m_Peer = p_Peer;
        m_Api->setId(&m_Identity, m_Peer);
        m_Accepted = false;
    }

    std::shared_ptr<Api> m_Api;
    Id m_Lobby;
    Id m_Local;
    Id m_Host;
    std::shared_ptr<Id> m_Hosted;
    Id m_Peer = 0;
    Identity m_Identity;
    bool m_Accepted = false;
};
} // namespace

struct SteamCoop::Impl {
    std::shared_ptr<Api> api = std::make_shared<Api>();
    std::shared_ptr<Id> hosted = std::make_shared<Id>(0);
    Callback invited { c_JoinRequested, sizeof(JoinRequested) };
    Callback created { c_LobbyCreated, sizeof(Created) };
    Callback entered { c_LobbyEntered, sizeof(Entered) };
    Id pending = 0;
    bool loading = false;
    uint64_t nextLoad = 0;
    bool registered = false;
    bool online = false;
    bool canceled = false;
    Transfer mode = Transfer::None;
    std::string notice;

    ~Impl() {
        if (registered)
            api->unregisterCallback(&invited);
        if (pending)
            api->unregisterResult(mode == Transfer::Host ? &created : &entered, pending);
    }
};

SteamCoop::SteamCoop() : m_Impl(std::make_unique<Impl>()) {}
SteamCoop::~SteamCoop() = default;
bool SteamCoop::Available() const { return m_Impl->online; }
bool SteamCoop::Pending() const { return m_Impl->pending && !m_Impl->canceled; }
bool SteamCoop::Hosting() const { return *m_Impl->hosted != 0; }
std::string SteamCoop::TakeNotice() { return std::exchange(m_Impl->notice, {}); }

void SteamCoop::Host() {
    auto& s = *m_Impl;
    if (s.pending || Hosting())
        return;
    if (!Available() || !s.api->overlayEnabled(s.api->utils)) {
        s.notice = "Steam hosting requires a connection to Steam and the in-game Steam Overlay.";
        return;
    }
    s.created.result = -1;
    s.pending = s.api->create(s.api->matchmaking, c_PrivateLobby, 2);
    if (!s.pending) {
        s.notice = "Steam could not start creating a lobby.";
        return;
    }
    s.canceled = false;
    s.mode = Transfer::Host;
    s.api->registerResult(&s.created, s.pending);
    s.notice = "Creating a private Steam lobby...";
}

void SteamCoop::Cancel() {
    if (m_Impl->pending)
        m_Impl->canceled = true;
}

void SteamCoop::Invite() {
    auto& s = *m_Impl;
    if (Hosting() && Available() && s.api->overlayEnabled(s.api->utils))
        s.api->invite(s.api->friends, *s.hosted);
    else
        s.notice = "Open a Steam lobby with HOST VIA STEAM and make sure Steam Overlay is enabled.";
}

SteamCoop::Ready SteamCoop::Update(bool p_CanJoin) {
    auto& s = *m_Impl;
    if (!s.registered) {
        const auto s_Tick = GetTickCount64();
        if (s_Tick < s.nextLoad)
            return {};
        s.nextLoad = s_Tick + 1000;
        if (!s.api->Load())
            return {};
        s.api->registerCallback(&s.invited, c_JoinRequested);
        s.registered = true;
        s.loading = true;
    }
    s.online = s.api->loggedOn(s.api->user);
    if (s.online && std::exchange(s.loading, false)) {
        char s_Line[1024] {};
        s.api->launchLine(s.api->apps, s_Line, sizeof(s_Line));
        s_Line[sizeof(s_Line) - 1] = '\0';
        Id s_Lobby = LaunchLobby(s_Line);
        if (!s_Lobby)
            s_Lobby = LaunchLobby(GetCommandLineA());
        if (s_Lobby) {
            Id s_Empty = 0;
            s.invited.lobby.compare_exchange_strong(s_Empty, s_Lobby);
        }
    }

    const Id s_Invite = s.online ? s.invited.lobby.exchange(0) : 0;
    if (s_Invite && !s.pending && p_CanJoin) {
        s.entered.result = -1;
        s.pending = s.api->join(s.api->matchmaking, s_Invite);
        if (s.pending) {
            s.mode = Transfer::Join;
            s.canceled = false;
            s.api->registerResult(&s.entered, s.pending);
            s.notice = "Joining the Steam lobby...";
        }
        else
            s.notice = "Steam could not start joining the lobby.";
    }
    else if (s_Invite)
        s.notice = "Leave your current co-op session and accept the Steam invitation again.";

    if (!s.pending)
        return {};
    const bool s_Hosting = s.mode == Transfer::Host;
    Callback& s_Result = s_Hosting ? s.created : s.entered;
    const int s_Code = s_Result.result.load(std::memory_order_acquire);
    if (s_Code == -1)
        return {};
    const Id s_Lobby = s_Result.lobby.load(std::memory_order_relaxed);
    const Id s_Call = std::exchange(s.pending, 0);
    const Transfer s_Mode = std::exchange(s.mode, Transfer::None);
    if (s_Code != c_Ok || !s_Lobby) {
        if (!s.canceled) {
            const std::string s_Error = s_Code == -2 ? "Steam API failure " +
                std::to_string(s.api->callFailure(s.api->utils, s_Call)) : "result " + std::to_string(s_Code);
            s.notice = std::string(s_Hosting ? "Steam could not create the lobby" : "Steam could not join the lobby") +
                " (" + s_Error + ", app " + std::to_string(s.api->appId(s.api->utils)) + ").";
        }
        return {};
    }
    const Id s_Local = s.api->steamId(s.api->user);
    const Id s_Owner = s.api->owner(s.api->matchmaking, s_Lobby);
    const char* s_Version = s_Hosting ? nullptr : s.api->getData(s.api->matchmaking, s_Lobby, c_LobbyKey);
    if (s.canceled || !s_Local || !s_Owner || (s_Hosting ? s_Owner != s_Local ||
        !s.api->setData(s.api->matchmaking, s_Lobby, c_LobbyKey, c_LobbyVersion) :
        s_Owner == s_Local || !s_Version || std::strcmp(s_Version, c_LobbyVersion) != 0)) {
        s.api->leave(s.api->matchmaking, s_Lobby);
        if (!s.canceled)
            s.notice = s_Hosting ? "Steam could not configure the lobby." : "That Steam lobby is not a compatible HITMAN Co-op lobby.";
        return {};
    }
    if (s_Hosting) {
        *s.hosted = s_Lobby;
        s.notice = "Steam lobby ready. Use Invite Steam friend in CO-OP.";
    }
    else
        s.notice = "Connecting to the host over Steam...";

    return { s_Mode, std::make_unique<SteamSocket>(s.api, s_Lobby, s_Local, s_Owner, s.hosted), coop::Endpoint::Steam(s_Owner) };
}
