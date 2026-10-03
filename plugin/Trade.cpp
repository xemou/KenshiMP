// Direct trade between two players, in the game's own trade window.
//
// A asks (ÉCHANGER button of B's row in the Multiplayer window, chat notice on B's side), B accepts
// with the same button. Each side then opens the game's trade window between its character and
// the other's ghost (the pair standing closest, within reach). Items dragged across go through the
// normal validated transfers (Items.cpp: MSG_ITEM_TAKE / MSG_ITEM_GIVE to the real owner), so
// nothing is duplicated and both windows converge through the owners' inventory updates.
// The window closing on one side ends the trade on both.
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/Character.h>
#include <kenshi/Inventory.h>
#include <kenshi/gui/ForgottenGUI.h>
#include <kenshi/util/hand.h>

#include "Shared.h"

#include <map>

using namespace mp;

namespace kmp {

namespace
{
    enum { TRADE_REQUEST = 0, TRADE_ACCEPT = 1, TRADE_END = 2 };
    const float TRADE_RANGE = 25.f;          // a little under the owners' transfer check (30)
    const DWORD REQUEST_TTL_MS = 60000;
    const DWORD OPEN_TIMEOUT_MS = 3000;

    struct Request { uint32_t theirs, mine; DWORD at; };
    std::map<uint8_t, Request> g_incoming;   // asked by that player
    std::map<uint8_t, Request> g_outgoing;   // we asked that player

    struct Open { uint8_t player; hand mine, ghost; DWORD at; bool seen; };
    bool g_open = false;
    Open g_trade;

    void send(uint8_t player, uint8_t action, uint32_t mine, uint32_t theirs)
    {
        ByteWriter w; w.u8(player); w.u8(action); w.u32(mine); w.u32(theirs);
        g_session.send(MSG_TRADE, w.data);
    }

    // Our character and that player's ghost standing closest to each other.
    bool closestPair(uint8_t player, Character** mine, Character** ghost)
    {
        std::vector<std::pair<uint32_t, Character*> > locals;
        chars_localCharacters(locals);
        float best = TRADE_RANGE;
        *mine = *ghost = NULL;
        for (size_t i = 0; i < locals.size(); ++i)
        {
            Character* c = locals[i].second;
            if (!c) continue;
            Character* g = chars_ghostNear(player, c->getPosition(), best);
            if (g) { best = g->getPosition().distance(c->getPosition()); *mine = c; *ghost = g; }
        }
        return *mine != NULL;
    }

    bool safeShow(Character* mine, Character* ghost)
    {
        __try { gui->showTradeWindow(hand(mine), hand(ghost), TW_LOOTING); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeVisible(Character* c)
    {
        __try { return c->inventory && c->inventory->isVisible(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void safeClose()
    {
        __try { gui->closeTradeWindow(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void openWith(uint8_t player, Character* mine, Character* ghost)
    {
        if (g_open && g_trade.player != player) { send(g_trade.player, TRADE_END, 0, 0); safeClose(); }
        if (!safeShow(mine, ghost)) { log("trade: could not open the trade window"); return; }
        g_open = true;
        g_trade.player = player; g_trade.mine = hand(mine); g_trade.ghost = hand(ghost);
        g_trade.at = GetTickCount(); g_trade.seen = false;
        log("trade with %s opened (%s <-> %s)", playerName(player).c_str(), mine->getName().c_str(), ghost->getName().c_str());
    }
}

bool trade_pendingFrom(uint8_t player)
{
    std::map<uint8_t, Request>::iterator it = g_incoming.find(player);
    return it != g_incoming.end() && GetTickCount() - it->second.at < REQUEST_TTL_MS;
}

// ÉCHANGER button / debug key: accept that player's request, or ask them.
void trade_request(uint8_t player)
{
    if (!ready() || !g_session.active() || player == g_session.localId()) return;
    Character* mine = NULL; Character* ghost = NULL;
    if (!closestPair(player, &mine, &ghost))
    {
        chat_notice(TF("Walk up to one of %s's characters to trade.", playerName(player).c_str()));
        return;
    }
    uint32_t myId = chars_netIdOf(mine), theirId = chars_netIdOf(ghost);
    if (trade_pendingFrom(player))
    {
        g_incoming.erase(player);
        send(player, TRADE_ACCEPT, myId, theirId);
        openWith(player, mine, ghost);
        return;
    }
    Request r; r.mine = myId; r.theirs = theirId; r.at = GetTickCount();
    g_outgoing[player] = r;
    send(player, TRADE_REQUEST, myId, theirId);
    chat_notice(TF("Trade offer sent to %s.", playerName(player).c_str()));
}

void trade_onMessage(const NetEvent& e)
{
    ByteReader r(e.body);
    uint8_t target = r.u8(), action = r.u8();
    uint32_t theirs = r.u32(), mine = r.u32();
    if (!r.ok() || target != g_session.localId() || !ready()) return;
    if (action == TRADE_REQUEST)
    {
        Request q; q.theirs = theirs; q.mine = mine; q.at = GetTickCount();
        g_incoming[e.sender] = q;
        chat_notice(TF("%s offers to trade: F4, then ACCEPT on their line.", playerName(e.sender).c_str()));
        showMessage(TF("%s offers to trade.", playerName(e.sender).c_str()));
    }
    else if (action == TRADE_ACCEPT)
    {
        std::map<uint8_t, Request>::iterator it = g_outgoing.find(e.sender);
        if (it == g_outgoing.end() || GetTickCount() - it->second.at > REQUEST_TTL_MS) return;   // not asked (any more)
        g_outgoing.erase(it);
        // The pair they chose (they may have picked another of our characters).
        Character* c = chars_byNetId(mine);
        Character* g = chars_byNetId(theirs);
        if (!c || !g || chars_isGhost(c) || c->getPosition().distance(g->getPosition()) > TRADE_RANGE + 5.f)
            if (!closestPair(e.sender, &c, &g)) { chat_notice(TF("Walk up to one of %s's characters to trade.", playerName(e.sender).c_str())); return; }
        chat_notice(TF("%s accepted the trade.", playerName(e.sender).c_str()));
        openWith(e.sender, c, g);
    }
    else if (action == TRADE_END)
    {
        if (g_open && g_trade.player == e.sender) { g_open = false; safeClose(); chat_notice(TF("%s ended the trade.", playerName(e.sender).c_str())); }
    }
}

void trade_tick(DWORD now)
{
    if (!g_open) return;
    Character* mine = g_trade.mine.getCharacter();
    Character* ghost = g_trade.ghost.getCharacter();
    bool visible = ghost && safeVisible(ghost);
    if (visible) { g_trade.seen = true; }
    bool tooFar = mine && ghost && mine->getPosition().distance(ghost->getPosition()) > 40.f;
    if (!mine || !ghost || tooFar || (g_trade.seen && !visible) || (!g_trade.seen && now - g_trade.at > OPEN_TIMEOUT_MS))
    {
        if (!g_trade.seen && now - g_trade.at > OPEN_TIMEOUT_MS) log("trade: the window did not show the other character");
        g_open = false;
        if (visible || tooFar) safeClose();
        send(g_trade.player, TRADE_END, 0, 0);
        log("trade with %s ended", playerName(g_trade.player).c_str());
    }
}

void trade_endAll()
{
    if (!g_open) return;
    g_open = false;
    safeClose();
    send(g_trade.player, TRADE_END, 0, 0);
    log("trade with %s ended", playerName(g_trade.player).c_str());
}

void trade_onPlayerLeft(uint8_t id)
{
    g_incoming.erase(id);
    g_outgoing.erase(id);
    if (g_open && g_trade.player == id) { g_open = false; safeClose(); }
}

void trade_onWorldReload()
{
    g_incoming.clear();
    g_outgoing.clear();
    g_open = false;
}

} // namespace kmp
