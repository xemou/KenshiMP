// Steam: play over the Internet without an IP address, an open port or a VPN.
//
// Kenshi (Steam version) ships steam_api64.dll and initialises Steam itself; we only call its
// exported flat functions (resolved at run time, no Steam SDK file needed):
//  - ISteamNetworking (legacy P2P): reliable messages addressed by Steam account, with NAT
//    traversal and Valve's relays when a direct path is impossible. It carries the session's
//    TCP stream through core/Tunnel (the session itself is unchanged);
//  - ISteamFriends: the friends list, invitations (InviteUserToGame) and the "Join game" entry of
//    the host's profile (rich presence "connect");
//  - callbacks: incoming P2P session requests, and join requests from an invitation or a profile.
// A GOG copy (or Steam not running) simply has no Steam option: IP connections still work.
#include "Shared.h"
#include "../core/Tunnel.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <vector>

using namespace mp;

namespace kmp {

namespace
{
    // ---------------------------------------------------------------- flat API (steam_api64.dll)
    typedef void* (*CreateInterfaceFn)(const char*);
    typedef int (*GetHandleFn)();
    typedef void* (*GetInterfaceFn)(void* client, int user, int pipe, const char* version);
    typedef void* (*GetUserInterfaceFn)(void* client, int user, int pipe, const char* version);
    typedef unsigned long long (*GetSteamIDFn)(void* user);
    typedef bool (*SendP2PFn)(void* net, unsigned long long peer, const void* data, unsigned int size, int sendType, int channel);
    typedef bool (*IsP2PAvailableFn)(void* net, unsigned int* size, int channel);
    typedef bool (*ReadP2PFn)(void* net, void* dest, unsigned int destSize, unsigned int* size, unsigned long long* peer, int channel);
    typedef bool (*PeerFn)(void* net, unsigned long long peer);
    typedef bool (*AllowRelayFn)(void* net, bool allow);
    typedef int (*FriendCountFn)(void* friends, int flags);
    typedef unsigned long long (*FriendByIndexFn)(void* friends, int index, int flags);
    typedef const char* (*FriendNameFn)(void* friends, unsigned long long id);
    typedef int (*FriendStateFn)(void* friends, unsigned long long id);
    typedef bool (*FriendGameFn)(void* friends, unsigned long long id, void* info);
    typedef bool (*InviteFn)(void* friends, unsigned long long id, const char* connect);
    typedef bool (*RichPresenceFn)(void* friends, const char* key, const char* value);
    typedef const char* (*PersonaFn)(void* friends);
    typedef void (*RunCallbacksFn)();
    typedef void (*RegisterCallbackFn)(void* callback, int id);

    struct Api
    {
        HMODULE dll;
        void* client; void* net; void* friends; void* user;
        int hUser, hPipe;
        GetSteamIDFn getSteamID;
        SendP2PFn sendP2P; IsP2PAvailableFn isP2PAvailable; ReadP2PFn readP2P;
        PeerFn acceptSession, closeSession; AllowRelayFn allowRelay;
        FriendCountFn friendCount; FriendByIndexFn friendByIndex; FriendNameFn friendName;
        FriendStateFn friendState; FriendGameFn friendGame; InviteFn invite; RichPresenceFn richPresence;
        PersonaFn persona; RunCallbacksFn runCallbacks; RegisterCallbackFn registerCallback;
    };
    Api g_api;
    bool g_tried = false, g_ok = false;
    unsigned long long g_me = 0;
    const unsigned int KENSHI_APPID = 233860;

    template <class T> bool resolve(T& fn, const char* name)
    {
        fn = (T)GetProcAddress(g_api.dll, name);
        if (!fn) log("steam: %s not found in steam_api64.dll", name);
        return fn != NULL;
    }

    // ---------------------------------------------------------------- callbacks
    // Same layout as the SDK's CCallbackBase (built with the same compiler family): Steam calls Run.
    class CallbackBase
    {
    public:
        CallbackBase() : flags(0), id(0) {}
        virtual void Run(void* param) = 0;
        virtual void Run(void* param, bool ioFailure, unsigned long long call) = 0;
        virtual int GetCallbackSizeBytes() = 0;
        unsigned char flags;
        int id;
    };

    CRITICAL_SECTION g_cs;
    std::deque<unsigned long long> g_joinRequests;   // host Steam ids to join (invitation / profile)

    bool parseJoin(const char* connect, unsigned long long& host)
    {
        const char* p = connect ? strstr(connect, "+kmp_join") : NULL;
        if (!p) return false;
        p += 9;
        while (*p == ' ') ++p;
        host = _strtoui64(p, NULL, 10);
        return host != 0;
    }

    // P2PSessionRequest_t (1202): someone opens a P2P session with us (a client of our session).
    class SessionRequestCb : public CallbackBase
    {
    public:
        void Run(void* param)
        {
            unsigned long long who = *(unsigned long long*)param;
            if (g_api.acceptSession) g_api.acceptSession(g_api.net, who);
            log("steam: P2P session accepted from %llu", who);
        }
        void Run(void* param, bool, unsigned long long) { Run(param); }
        int GetCallbackSizeBytes() { return 8; }
    };
    // P2PSessionConnectFail_t (1203)
    class ConnectFailCb : public CallbackBase
    {
    public:
        void Run(void* param)
        {
            unsigned long long who = *(unsigned long long*)param;
            unsigned char err = *((unsigned char*)param + 8);
            log("steam: P2P session with %llu failed (error %d)", who, (int)err);
        }
        void Run(void* param, bool, unsigned long long) { Run(param); }
        int GetCallbackSizeBytes() { return 16; }
    };
    // GameRichPresenceJoinRequested_t (337): an invitation was accepted / "Join game" clicked.
    class JoinRequestCb : public CallbackBase
    {
    public:
        void Run(void* param)
        {
            const char* connect = (const char*)param + 8;
            unsigned long long host = 0;
            if (parseJoin(connect, host)) { EnterCriticalSection(&g_cs); g_joinRequests.push_back(host); LeaveCriticalSection(&g_cs); }
            log("steam: join request '%s'", connect);
        }
        void Run(void* param, bool, unsigned long long) { Run(param); }
        int GetCallbackSizeBytes() { return 8 + 256; }
    };
    SessionRequestCb g_cbSession;
    ConnectFailCb g_cbFail;
    JoinRequestCb g_cbJoin;

    // ---------------------------------------------------------------- transport over Steam P2P
    class SteamP2P : public P2PTransport
    {
    public:
        bool send(uint64_t peer, const uint8_t* data, uint32_t size)
        {
            return g_api.sendP2P(g_api.net, peer, data, size, 2 /* k_EP2PSendReliable */, 0);
        }
        bool receive(uint64_t& peer, Bytes& out)
        {
            unsigned int size = 0;
            if (!g_api.isP2PAvailable(g_api.net, &size, 0) || size == 0) return false;
            out.resize(size);
            unsigned long long from = 0;
            if (!g_api.readP2P(g_api.net, &out[0], size, &size, &from, 0)) return false;
            out.resize(size);
            peer = from;
            return true;
        }
        void close(uint64_t peer) { g_api.closeSession(g_api.net, peer); }
    };
    SteamP2P g_transport;
    TunnelHost g_tunnelHost;
    TunnelClient g_tunnelClient;
    unsigned long long g_tunnelClientHost = 0;
}

bool steam_init()
{
    if (g_tried) return g_ok;
    g_tried = true;
    InitializeCriticalSection(&g_cs);
    memset(&g_api, 0, sizeof(g_api));
    g_api.dll = GetModuleHandleA("steam_api64.dll");
    if (!g_api.dll) { log("steam: not available (no steam_api64.dll: GOG copy?) - IP connections only"); return false; }
    CreateInterfaceFn createInterface = (CreateInterfaceFn)GetProcAddress(g_api.dll, "SteamInternal_CreateInterface");
    GetHandleFn getUser = (GetHandleFn)GetProcAddress(g_api.dll, "SteamAPI_GetHSteamUser");
    GetHandleFn getPipe = (GetHandleFn)GetProcAddress(g_api.dll, "SteamAPI_GetHSteamPipe");
    GetInterfaceFn getNet = (GetInterfaceFn)GetProcAddress(g_api.dll, "SteamAPI_ISteamClient_GetISteamNetworking");
    GetInterfaceFn getFriends = (GetInterfaceFn)GetProcAddress(g_api.dll, "SteamAPI_ISteamClient_GetISteamFriends");
    GetUserInterfaceFn getUserIf = (GetUserInterfaceFn)GetProcAddress(g_api.dll, "SteamAPI_ISteamClient_GetISteamUser");
    if (!createInterface || !getUser || !getPipe || !getNet || !getFriends || !getUserIf) { log("steam: entry points missing"); return false; }
    g_api.client = createInterface("SteamClient017");
    g_api.hUser = getUser(); g_api.hPipe = getPipe();
    if (!g_api.client || !g_api.hUser || !g_api.hPipe) { log("steam: not running / not initialised by the game"); return false; }
    g_api.net = getNet(g_api.client, g_api.hUser, g_api.hPipe, "SteamNetworking005");
    g_api.friends = getFriends(g_api.client, g_api.hUser, g_api.hPipe, "SteamFriends015");
    g_api.user = getUserIf(g_api.client, g_api.hUser, g_api.hPipe, "SteamUser019");
    if (!g_api.net || !g_api.friends || !g_api.user) { log("steam: interfaces unavailable (net %p friends %p user %p)", g_api.net, g_api.friends, g_api.user); return false; }
    bool all = resolve(g_api.getSteamID, "SteamAPI_ISteamUser_GetSteamID")
        & resolve(g_api.sendP2P, "SteamAPI_ISteamNetworking_SendP2PPacket")
        & resolve(g_api.isP2PAvailable, "SteamAPI_ISteamNetworking_IsP2PPacketAvailable")
        & resolve(g_api.readP2P, "SteamAPI_ISteamNetworking_ReadP2PPacket")
        & resolve(g_api.acceptSession, "SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser")
        & resolve(g_api.closeSession, "SteamAPI_ISteamNetworking_CloseP2PSessionWithUser")
        & resolve(g_api.allowRelay, "SteamAPI_ISteamNetworking_AllowP2PPacketRelay")
        & resolve(g_api.friendCount, "SteamAPI_ISteamFriends_GetFriendCount")
        & resolve(g_api.friendByIndex, "SteamAPI_ISteamFriends_GetFriendByIndex")
        & resolve(g_api.friendName, "SteamAPI_ISteamFriends_GetFriendPersonaName")
        & resolve(g_api.friendState, "SteamAPI_ISteamFriends_GetFriendPersonaState")
        & resolve(g_api.friendGame, "SteamAPI_ISteamFriends_GetFriendGamePlayed")
        & resolve(g_api.invite, "SteamAPI_ISteamFriends_InviteUserToGame")
        & resolve(g_api.richPresence, "SteamAPI_ISteamFriends_SetRichPresence")
        & resolve(g_api.persona, "SteamAPI_ISteamFriends_GetPersonaName")
        & resolve(g_api.runCallbacks, "SteamAPI_RunCallbacks")
        & resolve(g_api.registerCallback, "SteamAPI_RegisterCallback");
    if (!all) return false;
    g_me = g_api.getSteamID(g_api.user);
    g_api.allowRelay(g_api.net, true);
    g_cbSession.id = 1202; g_api.registerCallback(&g_cbSession, 1202);
    g_cbFail.id = 1203;    g_api.registerCallback(&g_cbFail, 1203);
    g_cbJoin.id = 337;     g_api.registerCallback(&g_cbJoin, 337);
    g_ok = g_me != 0;
    log("steam: ready as '%s' (%llu), %d friend(s)", g_api.persona(g_api.friends), g_me, g_api.friendCount(g_api.friends, 4));
    // Started by Steam from an invitation ("+kmp_join <host>" on the command line).
    unsigned long long host = 0;
    if (g_ok && parseJoin(GetCommandLineA(), host)) { EnterCriticalSection(&g_cs); g_joinRequests.push_back(host); LeaveCriticalSection(&g_cs); }
    return g_ok;
}

bool steam_available() { return g_ok; }
unsigned long long steam_myId() { return g_me; }
std::string steam_personaName() { return g_ok ? std::string(g_api.persona(g_api.friends)) : std::string(); }

void steam_friends(std::vector<SteamFriend>& out)
{
    out.clear();
    if (!g_ok) return;
    int n = g_api.friendCount(g_api.friends, 4 /* k_EFriendFlagImmediate */);
    for (int i = 0; i < n && i < 500; ++i)
    {
        SteamFriend f;
        f.id = g_api.friendByIndex(g_api.friends, i, 4);
        if (!f.id) continue;
        f.online = g_api.friendState(g_api.friends, f.id) != 0;
        const char* name = g_api.friendName(g_api.friends, f.id);
        f.name = name ? name : "?";
        unsigned char info[64]; memset(info, 0, sizeof(info));   // FriendGameInfo_t: CGameID first
        f.inKenshi = g_api.friendGame(g_api.friends, f.id, info) && ((*(unsigned long long*)info) & 0xFFFFFF) == KENSHI_APPID;
        out.push_back(f);
    }
}

std::string steam_connectString()
{
    char buf[64]; sprintf_s(buf, "+kmp_join %llu", g_me);
    return buf;
}

bool steam_invite(unsigned long long friendId)
{
    if (!g_ok) return false;
    bool ok = g_api.invite(g_api.friends, friendId, steam_connectString().c_str());
    log("steam: invitation to %llu %s", friendId, ok ? "sent" : "failed");
    return ok;
}

// Host side: the tunnel takes Steam players to our session port; friends see "Join game".
void steam_onHosting(bool hosting, int port)
{
    if (!g_ok) return;
    if (hosting)
    {
        std::string err;
        if (!g_tunnelHost.running() && !g_tunnelHost.start(&g_transport, port, err)) log("steam: tunnel host failed: %s", err.c_str());
        g_api.richPresence(g_api.friends, "connect", steam_connectString().c_str());
        g_api.richPresence(g_api.friends, "status", "KenshiMP");
        log("steam: hosting, friends can join through Steam");
    }
    else
    {
        g_tunnelHost.stop();
        g_api.richPresence(g_api.friends, "connect", "");
    }
}

// Client side: the local port the session must join to reach that Steam host (0: failed).
int steam_tunnelTo(unsigned long long host)
{
    if (!g_ok || !host) return 0;
    if (g_tunnelClient.running() && g_tunnelClientHost == host && !g_tunnelClient.hostClosed()) return g_tunnelClient.localPort();
    std::string err;
    g_tunnelClientHost = host;
    if (!g_tunnelClient.start(&g_transport, host, err)) { log("steam: tunnel to %llu failed: %s", host, err.c_str()); return 0; }
    log("steam: tunnel to %llu on 127.0.0.1:%d", host, g_tunnelClient.localPort());
    return g_tunnelClient.localPort();
}

void steam_leave()
{
    if (!g_ok) return;
    g_tunnelClient.stop();
    g_tunnelHost.stop();
    g_api.richPresence(g_api.friends, "connect", "");
}

// Every frame (game thread): Steam callbacks, then a pending join request, if any.
bool steam_takeJoinRequest(unsigned long long& host)
{
    if (!g_ok) return false;
    g_api.runCallbacks();
    EnterCriticalSection(&g_cs);
    bool any = !g_joinRequests.empty();
    if (any) { host = g_joinRequests.front(); g_joinRequests.pop_front(); }
    LeaveCriticalSection(&g_cs);
    return any;
}

} // namespace kmp
