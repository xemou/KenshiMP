// World sync: the host is the clock. Every 5 s it broadcasts its in-game time; clients whose clock
// drifted by more than ~1 in-game minute snap to it.
//
// The engine keeps the world time as a double at [[global] + disp] and GameWorld::
// getTimeStamp_inGameHours is just:   mov rax,[rip+rel32] ; mov rcx,[rax+disp32] ; ...
// We decode those two instructions at runtime to find the variable (version independent).
#include <Debug.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/SaveManager.h>
#include <core/Functions.h>

#include "Shared.h"

#include <string.h>

using namespace mp;

namespace kmp {

namespace
{
    const DWORD SYNC_PERIOD_MS = 5000;
    const double MAX_DRIFT_HOURS = 1.0 / 60.0;
    DWORD g_lastSync = 0;

    void** g_clockHolder = NULL;   // address of the global pointer
    int32_t g_clockOffset = 0;     // offset of the double inside the pointed object

    double* clock()
    {
        if (!g_clockHolder || !*g_clockHolder) return NULL;
        return (double*)((char*)*g_clockHolder + g_clockOffset);
    }

    double safeRead()
    {
        __try { double* c = clock(); return c ? *c : -1; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }
    // The double is recomputed every frame (engine 1.0.65, time object update):
    //     hours = (double)day * 24 + skyController->hourOfDay
    // with   int day at [obj+0x08]   and   float hourOfDay at [[obj+0x20]+0x1C].
    // So we write those masters (and the cached double for the current frame).
    bool safeWrite(double v)
    {
        __try
        {
            if (!g_clockHolder || !*g_clockHolder || v < 0) return false;
            char* obj = (char*)*g_clockHolder;
            char* sky = *(char**)(obj + 0x20);
            if (!sky) return false;
            int day = (int)(v / 24.0);
            *(int*)(obj + 0x08) = day;
            *(float*)(sky + 0x1C) = (float)(v - day * 24.0);
            *(double*)(obj + g_clockOffset) = v;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

} // namespace kmp

// --- hook: SaveManager::save -------------------------------------------------------------------
// A connected client's world is not a normal world (its NPCs were replaced by the host's, other
// players are ghosts). Saving it over the player's solo save would damage that save, so a client
// always saves to "<name>_MP" instead; its solo game stays untouched.
void (*save_orig)(SaveManager*, const std::string&, bool) = NULL;
void save_hook(SaveManager* self, const std::string& name, bool autosave)
{
    using namespace kmp;
    if (ready() && !g_session.isHost())
    {
        const std::string suffix = "_MP";
        bool tagged = name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
        if (!tagged)
        {
            std::string mpName = name + suffix;
            log("client save redirected: '%s' -> '%s'", name.c_str(), mpName.c_str());
            if (!autosave) showMessage(TF("Multiplayer: saved as '%s' (your solo save is kept intact).", mpName.c_str()));
            save_orig(self, mpName, autosave);
            return;
        }
    }
    save_orig(self, name, autosave);
}

namespace kmp {

bool world_install()
{
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&SaveManager::save), save_hook, &save_orig))
        ErrorLog("KenshiMP: could not hook SaveManager::save (client saves are not redirected)");

    const unsigned char* f = (const unsigned char*)KenshiLib::GetRealAddress(&GameWorld::getTimeStamp_inGameHours);
    // 48 8B 05 rel32        mov rax, [rip+rel32]
    // 48 8B 88 disp32       mov rcx, [rax+disp32]
    if (f && f[0] == 0x48 && f[1] == 0x8B && f[2] == 0x05 && f[7] == 0x48 && f[8] == 0x8B && f[9] == 0x88)
    {
        int32_t rel, disp;
        memcpy(&rel, f + 3, 4);
        memcpy(&disp, f + 10, 4);
        g_clockHolder = (void**)(f + 7 + rel);
        g_clockOffset = disp;
        log("world clock located (holder %p, offset 0x%x)", g_clockHolder, disp);
        return true;
    }
    ErrorLog("KenshiMP: world clock not found, time sync disabled");
    return false;
}

namespace
{
    const uint32_t FLAG_PAUSED = 1;
    bool g_hostPaused = false;        // client: last pause state announced by the host
    bool g_lastSentPaused = false;    // host: last pause state we announced
    DWORD g_lastPauseNotice = 0;

    bool safeIsPaused()
    {
        __try { return ou->isPaused(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void safeUserPause(bool p)
    {
        __try { ou->userPause(p); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void hostBroadcast(bool paused)
    {
        double h = safeRead();
        ByteWriter w; w.f64(h); w.u32(paused ? FLAG_PAUSED : 0);
        g_session.send(MSG_WORLD_SYNC, w.data);
        g_lastSentPaused = paused;
    }
}

// Host: the clock goes out at the next tick (a player just joined / reloaded its world).
void world_sendNow() { g_lastSync = 0; }

static void speedReset();
void world_reset()
{
    speedReset();
    g_hostPaused = false;   // a new session starts from the host's next announcement
}

void world_tick(DWORD now)
{
    if (g_session.isHost())
    {
        // Pause changes go out immediately, the clock every SYNC_PERIOD_MS.
        bool paused = g_cfg.pauseSync && safeIsPaused();
        if (paused != g_lastSentPaused || now - g_lastSync >= SYNC_PERIOD_MS)
        {
            g_lastSync = now;
            hostBroadcast(paused);
        }
        return;
    }
    // Client: the host owns pause. Undo a local pause the host did not ask for (a paused
    // client would drift away from the shared world) and follow the host's pauses.
    if (!g_cfg.pauseSync) return;
    bool mine = safeIsPaused();
    if (mine != g_hostPaused)
    {
        safeUserPause(g_hostPaused);
        if (!g_hostPaused && now - g_lastPauseNotice > 10000)
        {
            g_lastPauseNotice = now;
            showMessage(T("Pause is controlled by the host in multiplayer."));
        }
    }
}

// --------------------------------------------------------------------------- game speed
// Everybody simulates the same world, so the speed is common: each player chooses with the game's own buttons, and
// it applies only when ALL players chose the same one (x1 as long as they disagree). A player's choice is what the
// speed was set to by hand: when the game's speed differs from what we applied, the player changed it.
namespace
{
    std::map<uint8_t, float> g_wishes;     // other players' chosen speeds
    float g_myWish = 1.f, g_applied = 1.f;
    DWORD g_lastWishSent = 0, g_lastSpeedNotice = 0;
    const DWORD WISH_PERIOD_MS = 3000;

    float safeSpeedNow() { __try { return ou->getFrameSpeedMultiplier(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 1.f; } }
    void safeSetSpeed(float v) { __try { ou->setGameSpeed(v, false); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    float clampSpeed(float v) { return !(v == v) ? 1.f : (v < 1.f ? 1.f : (v > 4.f ? 4.f : v)); }
    void sendWish()
    {
        ByteWriter w; w.f32(g_myWish);
        g_session.send(MSG_SPEED, w.data);
        g_lastWishSent = GetTickCount();
    }
}

float world_debugSpeedWish() { return g_myWish; }
static void speedReset() { g_wishes.clear(); g_myWish = g_applied = 1.f; }

void world_onSpeed(const NetEvent& e)
{
    ByteReader r(e.body);
    float v = r.f32();
    if (!r.ok() || e.sender == g_session.localId()) return;
    g_wishes[e.sender] = clampSpeed(v);
}

void world_onPlayerLeft(uint8_t id) { g_wishes.erase(id); }

void world_speedTick(DWORD now)
{
    float cur = safeSpeedNow();
    if (fabsf(cur - g_applied) > 0.01f)   // the player used the speed buttons (or a load reset it)
    {
        g_myWish = clampSpeed(cur);
        sendWish();
    }
    else if (now - g_lastWishSent >= WISH_PERIOD_MS) sendWish();   // newcomers learn it too

    // Common speed: ours if every other player chose it too.
    float eff = g_myWish;
    std::string waiting;
    std::vector<PlayerInfo> ps = g_session.players();
    for (size_t i = 0; i < ps.size(); ++i)
    {
        if (ps[i].id == g_session.localId()) continue;
        std::map<uint8_t, float>::iterator w = g_wishes.find(ps[i].id);
        float theirs = w == g_wishes.end() ? 1.f : w->second;
        if (fabsf(theirs - g_myWish) > 0.01f) { eff = 1.f; if (!waiting.empty()) waiting += ", "; waiting += ps[i].name; }
    }
    if (fabsf(eff - cur) > 0.01f) safeSetSpeed(eff);
    g_applied = eff;
    if (eff < g_myWish - 0.01f && now - g_lastSpeedNotice > 15000)
    {
        g_lastSpeedNotice = now;
        showMessage(TF("Speed x%.0f needs everybody on it (waiting for: %s).", g_myWish, waiting.c_str()));
    }
}

// Debug (F8 with debug_keys=1): jump the local clock 3 h ahead to verify clock writes.
void world_debugShift()
{
    double h = safeRead();
    if (h >= 0 && safeWrite(h + 3.0)) log("debug: clock %.3f h -> %.3f h", h, h + 3.0);
    else log("debug: clock shift failed");
}

void world_onMessage(const NetEvent& e)
{
    if (g_session.isHost() || e.sender != HOST_ID) return;
    ByteReader r(e.body);
    double hostHours = r.f64();
    uint32_t flags = r.u32();
    if (!r.ok()) return;
    g_hostPaused = (flags & FLAG_PAUSED) != 0;
    if (!(hostHours >= 0 && hostHours < 1.0e7)) return;   // also rejects NaN
    double mine = safeRead();
    if (mine < 0) return;
    double diff = hostHours - mine;
    if (diff > MAX_DRIFT_HOURS || diff < -MAX_DRIFT_HOURS)
    {
        if (safeWrite(hostHours)) log("world clock synced to host (%.3f h, was %.3f h)", hostHours, mine);
        else log("world clock write failed");
    }
}

} // namespace kmp
