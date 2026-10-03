// Lobby window (lobby_key, default F4, or the MULTIPLAYER button of the title screen): name, faction, address, port, password,
// Host / Join / Disconnect, and a live status (players, ping). Choices are saved to kenshimp.cfg,
// so the next launch reconnects the same way.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Faction.h>
#include <kenshi/gui/ManagementScreen.h>
#include <kenshi/gui/FactionsScreen.h>
#include <mygui/MyGUI.h>

#include "Shared.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace mp;

namespace kmp {

namespace
{
    MyGUI::Window* g_win = NULL;
    MyGUI::EditBox *g_name = NULL, *g_faction = NULL, *g_address = NULL, *g_port = NULL, *g_password = NULL;
    MyGUI::EditBox* g_status = NULL;
    enum Action { NONE, HOST, JOIN, LEAVE, CLOSE };
    Action g_action = NONE;                 // widgets are never destroyed from their own events
    DWORD g_lastStatus = 0;
    std::string g_lastError;

    const int W = 480, ROW = 42, LABEL_W = 140, PAD = 18;
    const int DIP_ROWS = 7, DIP_ROW_H = 32;
    std::string g_localIps;   // this PC's IPv4 addresses (LAN, VPN), for the host to share

    std::string localAddresses()
    {
        char host[256];
        if (gethostname(host, sizeof(host)) != 0) return std::string();
        addrinfo hints; memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        addrinfo* res = NULL;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return std::string();
        std::string out;
        for (addrinfo* a = res; a; a = a->ai_next)
        {
            char buf[64];
            sockaddr_in* sin = (sockaddr_in*)a->ai_addr;
            if (!inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) continue;
            std::string ip = buf;
            if (ip.compare(0, 4, "127.") == 0 || out.find(ip) != std::string::npos) continue;
            if (!out.empty()) out += ", ";
            out += ip;
        }
        freeaddrinfo(res);
        return out;
    }

    // Diplomacy row buttons: "KMP_Dip_<player>_<w|p|a>"; applied from lobby_tick, never from the event.
    int g_dipPlayer = -1; float g_dipValue = 0;
    bool g_friendsMode = false;                      // the panel lists Steam friends (invitations)
    std::vector<unsigned long long> g_friendIds;     // row -> friend
    unsigned long long g_inviteId = 0;               // invitation to send (from lobby_tick)
    MyGUI::Widget* g_dipPanel = NULL;
    std::string g_dipShown;   // signature of what the rows show (players + relations)

    DWORD g_escapeClosedAt = 0;   // Escape closed us: the game may also open its pause menu with it
    void onKey(MyGUI::Widget*, MyGUI::KeyCode key, MyGUI::Char)
    {
        if (key == MyGUI::KeyCode::Escape) { g_action = CLOSE; g_escapeClosedAt = GetTickCount(); }
    }

    void onButton(MyGUI::Widget* w)
    {
        const std::string& n = w->getName();
        if (n.compare(0, 8, "KMP_Dip_") == 0 && n.size() > 10)
        {
            g_dipPlayer = atoi(n.c_str() + 8);
            char k = n[n.size() - 1];
            g_dipValue = k == 'w' ? -100.f : k == 'a' ? 100.f : k == 't' ? 1000.f : k == 'g' ? 2000.f : 0.f;   // 't': trade, 'g': go to
            return;
        }
        if (n == "KMP_SteamFriends") { g_friendsMode = !g_friendsMode; g_dipShown.clear(); return; }
        if (n.compare(0, 8, "KMP_Inv_") == 0)
        {
            size_t i = (size_t)atoi(n.c_str() + 8);
            if (i < g_friendIds.size()) g_inviteId = g_friendIds[i];
            return;
        }
        if (n == "KMP_Host") g_action = HOST;
        else if (n == "KMP_Join") g_action = JOIN;
        else if (n == "KMP_Leave") g_action = LEAVE;
        else if (n == "KMP_Close") g_action = CLOSE;
    }
    void onWindowButton(MyGUI::Window*, const std::string& name) { if (name == "close") g_action = CLOSE; }

    MyGUI::EditBox* field(MyGUI::Widget* parent, int row, const char* label, const std::string& value)
    {
        MyGUI::TextBox* t = parent->createWidget<MyGUI::TextBox>("Kenshi_TextboxStandardText",
            MyGUI::IntCoord(PAD, PAD + row * ROW, LABEL_W, 32), MyGUI::Align::Default);
        t->setCaption(T(label));
        MyGUI::EditBox* e = parent->createWidget<MyGUI::EditBox>("Kenshi_EditBox",
            MyGUI::IntCoord(PAD + LABEL_W, PAD + row * ROW, W - 2 * PAD - LABEL_W - 16, 32), MyGUI::Align::Default);
        e->setEditMultiLine(false);
        e->setMaxTextLength(64);
        e->setCaption(value);
        e->eventKeyButtonPressed += MyGUI::newDelegate(onKey);
        return e;
    }
    MyGUI::Button* button(MyGUI::Widget* parent, int x, int y, int w, const char* caption, const char* name)
    {
        MyGUI::Button* b = parent->createWidget<MyGUI::Button>("Kenshi_Button1", MyGUI::IntCoord(x, y, w, 36),
                                                                MyGUI::Align::Default, name);
        b->setCaption(T(caption));
        b->eventMouseButtonClick += MyGUI::newDelegate(onButton);
        return b;
    }

    // Defaults that feel like the player's own: Windows user name, in-game faction name.
    std::string defaultName()
    {
        if (!g_cfg.name.empty() && g_cfg.name != "Player") return g_cfg.name;
        char user[128]; DWORD n = sizeof(user);
        if (GetUserNameA(user, &n) && user[0]) return user;
        return g_cfg.name;
    }
    std::string defaultFaction()
    {
        if (!g_cfg.faction.empty() && g_cfg.faction != "My Faction") return g_cfg.faction;
        try
        {
            if (ou && ou->player && ou->player->playerCharacters.size() > 0)
                if (Faction* f = ou->player->getFaction())
                {
                    const std::string& n = f->getName();
                    if (!n.empty()) return n;
                }
        }
        catch (...) {}
        return g_cfg.faction;
    }

    bool create()
    {
        try
        {
            MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
            if (!gui) return false;
            const MyGUI::IntSize& view = MyGUI::RenderManager::getInstance().getViewSize();
            const int H = 560 + DIP_ROWS * DIP_ROW_H - 40 + (steam_available() ? 40 : 0);
            g_win = gui->createWidget<MyGUI::Window>("Kenshi_WindowCX", MyGUI::IntCoord((view.width - W) / 2, (view.height - H) / 2, W, H),
                                                     MyGUI::Align::Default, "Popup", "KenshiMP_Lobby");
            if (!g_win) return false;
            g_win->setCaption(std::string("KenshiMP - ") + T("Multiplayer"));
            g_win->eventWindowButtonPressed += MyGUI::newDelegate(onWindowButton);
            MyGUI::Widget* c = g_win->getClientWidget() ? g_win->getClientWidget() : g_win;

            char port[16]; sprintf_s(port, "%d", g_cfg.port);
            g_name = field(c, 0, "Name", defaultName());
            g_faction = field(c, 1, "Faction", defaultFaction());
            g_address = field(c, 2, "Host address", g_cfg.address);
            g_port = field(c, 3, "Port", port);
            g_password = field(c, 4, "Password", g_cfg.password);
            g_password->setEditPassword(true);

            int y = PAD + 5 * ROW + 6, bw = (W - 2 * PAD - 16 - 3 * 8) / 4;
            button(c, PAD, y, bw, "Host", "KMP_Host");
            button(c, PAD + (bw + 8), y, bw, "Join", "KMP_Join");
            button(c, PAD + 2 * (bw + 8), y, bw, "Leave", "KMP_Leave");
            button(c, PAD + 3 * (bw + 8), y, bw, "Close", "KMP_Close");
            if (steam_available()) button(c, PAD, y + 40, 2 * bw + 8, "Steam friends", "KMP_SteamFriends");
            if (steam_available()) y += 40;

            g_status = c->createWidget<MyGUI::EditBox>("Kenshi_WordWrap",
                MyGUI::IntCoord(PAD, y + 50, W - 2 * PAD - 16, 160), MyGUI::Align::Default);
            g_status->setEditMultiLine(true);
            g_status->setEditWordWrap(true);
            g_status->setEditReadOnly(true);
            g_status->setCaption("");
            g_dipPanel = c->createWidget<MyGUI::Widget>("", MyGUI::IntCoord(PAD, y + 50 + 168, W - 2 * PAD - 16, DIP_ROWS * DIP_ROW_H), MyGUI::Align::Default);
            g_dipShown.clear();
            MyGUI::InputManager::getInstance().setKeyFocusWidget(g_name);
            g_lastStatus = 0;
            return true;
        }
        catch (...) { g_win = NULL; return false; }
    }

    void destroy()
    {
        try
        {
            if (g_win)
            {
                MyGUI::InputManager::getInstance().resetKeyFocusWidget();
                MyGUI::Gui::getInstance().destroyWidget(g_win);
            }
        }
        catch (...) {}
        g_win = NULL; g_name = g_faction = g_address = g_port = g_password = g_status = NULL;
        g_dipPanel = NULL; g_dipShown.clear();
    }

    std::string text(MyGUI::EditBox* e)
    {
        std::string s = e ? e->getOnlyText().asUTF8() : std::string();
        size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }

    // Copies the fields into g_cfg. False (with a message) when something is unusable.
    bool readFields()
    {
        std::string name = text(g_name), faction = text(g_faction), address = text(g_address), port = text(g_port);
        int p = atoi(port.c_str());
        if (name.empty()) { g_lastError = T("Enter a name."); return false; }
        if (p < 1 || p > 65535) { g_lastError = T("Port must be between 1 and 65535."); return false; }
        g_cfg.name = name;
        g_cfg.faction = faction.empty() ? name + "'s faction" : faction;
        g_cfg.address = address.empty() ? "127.0.0.1" : address;
        g_cfg.port = p;
        g_cfg.password = text(g_password);
        return true;
    }

    void refreshStatus()
    {
        if (!g_status) return;
        std::string s;
        char buf[160];
        if (!g_session.active())
        {
            if (g_cfg.mode == "host") s = T("Not hosting yet: press Host (or load a game, kenshimp.cfg says mode=host).");
            else if (g_cfg.mode == "join") s = T("Not connected. Press Join (retries every 10 s once in game).");
            else s = T("Not connected. Host a game, or enter the host's address and press Join.");
        }
        else if (g_session.isHost())
        {
            s = TF("Hosting on port %d.", g_cfg.port);
            if (g_localIps.empty()) g_localIps = localAddresses();
            if (!g_localIps.empty()) s += TF(" This PC: %s.", g_localIps.c_str());
            s += std::string("\n") + T("Friends on your network/VPN (Radmin, ZeroTier, Tailscale) join with one of these addresses; over the Internet use your public IP and forward the TCP port on your router.");
            if (!ou || !ou->player || ou->player->playerCharacters.size() == 0) s += std::string("\n") + T("Start or load a game: players join your world.");
        }
        else if (!ready())
        {
            s = TF("Connecting to %s... (the connection completes once a game is loaded)", g_cfg.address.c_str());
        }
        else
        {
            s = TF("Connected to %s:%d - ping %d ms.", g_cfg.address.c_str(), g_cfg.port, g_session.pingMs());
        }
        std::vector<PlayerInfo> ps = g_session.players();
        if (!ps.empty())
        {
            s += std::string("\n") + T("Players:");
            for (size_t i = 0; i < ps.size(); ++i)
                s += "\n - " + ps[i].name + " (" + ps[i].faction + ")" + (ps[i].id == g_session.localId() ? T("  <- you") : "");
        }
        if (steam_available())
        {
            s += "\n" + TF("Steam: %s. ", steam_personaName().c_str());
            s += g_session.active() && g_session.isHost() ? T("Steam friends can join you: STEAM FRIENDS > INVITE, or \"Join game\" on your profile.")
                                                         : T("Accept a friend's Steam invitation to join them (no address needed).");
        }
        if (!g_lastError.empty()) s += "\n" + g_lastError;
        s += "\n" + TF("%s opens/closes this window. Enter = chat.", g_cfg.lobbyKeyName.c_str());
        // '#' starts a MyGUI colour code.
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) { if (s[i] == '#') out += '#'; out += s[i]; }
        try { g_status->setCaption(out); } catch (...) {}
    }
}

namespace
{
    // One row per other player: name, current standing, War / Peace / Ally.
    // Steam friends, online ones first and those playing Kenshi on top, each with INVITE.
    void refreshFriends()
    {
        std::vector<SteamFriend> all, rows;
        steam_friends(all);
        for (int pass = 0; pass < 2; ++pass)
            for (size_t i = 0; i < all.size() && rows.size() < (size_t)DIP_ROWS; ++i)
                if (all[i].online && all[i].inKenshi == (pass == 0)) rows.push_back(all[i]);
        std::string sig = "friends:";
        for (size_t i = 0; i < rows.size(); ++i) { char b[48]; sprintf_s(b, "%llu%d;", rows[i].id, (int)rows[i].inKenshi); sig += b; }
        if (sig == g_dipShown) return;
        g_dipShown = sig;
        try
        {
            while (g_dipPanel->getChildCount() > 0) MyGUI::Gui::getInstance().destroyWidget(g_dipPanel->getChildAt(0));
            g_friendIds.clear();
            int w = g_dipPanel->getWidth(), bw = 110;
            if (rows.empty())
            {
                MyGUI::TextBox* t = g_dipPanel->createWidget<MyGUI::TextBox>("Kenshi_TextboxStandardText", MyGUI::IntCoord(0, 0, w, DIP_ROW_H - 4), MyGUI::Align::Default);
                t->setCaption(T("No Steam friend online."));
                return;
            }
            for (size_t i = 0; i < rows.size(); ++i)
            {
                int y = (int)i * DIP_ROW_H;
                g_friendIds.push_back(rows[i].id);
                MyGUI::TextBox* t = g_dipPanel->createWidget<MyGUI::TextBox>("Kenshi_TextboxStandardText",
                    MyGUI::IntCoord(0, y, w - bw - 6, DIP_ROW_H - 4), MyGUI::Align::Default);
                std::string label = rows[i].name + (rows[i].inKenshi ? std::string(" - ") + T("in Kenshi") : std::string());
                std::string esc;
                for (size_t k = 0; k < label.size(); ++k) { if (label[k] == '#') esc += '#'; esc += label[k]; }
                t->setCaption(MyGUI::UString(esc));
                char name[32]; sprintf_s(name, "KMP_Inv_%d", (int)i);
                MyGUI::Button* b = g_dipPanel->createWidget<MyGUI::Button>("Kenshi_Button1",
                    MyGUI::IntCoord(w - bw, y, bw, DIP_ROW_H - 4), MyGUI::Align::Default, name);
                b->setCaption(T("Invite"));
                b->eventMouseButtonClick += MyGUI::newDelegate(onButton);
            }
        }
        catch (...) {}
    }

    void refreshDiplomacy()
    {
        if (!g_dipPanel) return;
        if (g_friendsMode) { refreshFriends(); return; }
        std::vector<PlayerInfo> ps = g_session.players();
        std::string sig;
        std::vector<std::pair<PlayerInfo, float> > rows;
        for (size_t i = 0; i < ps.size() && rows.size() < (size_t)DIP_ROWS; ++i)
        {
            if (ps[i].id == g_session.localId()) continue;
            float rel = diplomacy_relation(ps[i].id);
            rows.push_back(std::make_pair(ps[i], rel));
            char b[64]; sprintf_s(b, "%d:%s:%.0f:%d;", (int)ps[i].id, ps[i].name.c_str(), rel, (int)trade_pendingFrom(ps[i].id)); sig += b;
        }
        if (sig == g_dipShown) return;
        g_dipShown = sig;
        try
        {
            while (g_dipPanel->getChildCount() > 0) MyGUI::Gui::getInstance().destroyWidget(g_dipPanel->getChildAt(0));
            int w = g_dipPanel->getWidth(), bw = 60, gap = 4;
            for (size_t i = 0; i < rows.size(); ++i)
            {
                int y = (int)i * DIP_ROW_H;
                float rel = rows[i].second;
                const char* state = rel < 0 ? T("at war") : rel > 50 ? T("allied") : T("neutral");
                MyGUI::TextBox* t = g_dipPanel->createWidget<MyGUI::TextBox>("Kenshi_TextboxStandardText",
                    MyGUI::IntCoord(0, y, w - 5 * (bw + gap), DIP_ROW_H - 4), MyGUI::Align::Default);
                std::string label = rows[i].first.name + " - " + state;
                std::string esc;
                for (size_t k = 0; k < label.size(); ++k) { if (label[k] == '#') esc += '#'; esc += label[k]; }
                t->setCaption(esc);
                const char* caps[5] = { "War", "Peace", "Ally", trade_pendingFrom(rows[i].first.id) ? "Accept" : "Trade", "Go to" };
                const char kinds[5] = { 'w', 'p', 'a', 't', 'g' };
                for (int k = 0; k < 5; ++k)
                {
                    char name[48]; sprintf_s(name, "KMP_Dip_%d_%c", (int)rows[i].first.id, kinds[k]);
                    MyGUI::Button* b = g_dipPanel->createWidget<MyGUI::Button>("Kenshi_Button1",
                        MyGUI::IntCoord(w - (5 - k) * (bw + gap), y, bw, DIP_ROW_H - 4), MyGUI::Align::Default, name);
                    b->setCaption(T(caps[k]));
                    b->eventMouseButtonClick += MyGUI::newDelegate(onButton);
                }
            }
        }
        catch (...) {}
    }
}

namespace
{
    MyGUI::Button* g_titleButton = NULL;
    void onTitleButton(MyGUI::Widget*) { if (!g_win) create(); }
}

// Title screen: a MULTIPLAYER button under Kenshi's own menu (same skin, placed relative to the
// screen like the menu itself).
void lobby_titleButton(bool show)
{
    if (show == (g_titleButton != NULL)) return;
    try
    {
        if (!show)
        {
            MyGUI::Gui::getInstance().destroyWidget(g_titleButton);
            g_titleButton = NULL;
            return;
        }
        MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
        if (!gui) return;
        const MyGUI::IntSize& view = MyGUI::RenderManager::getInstance().getViewSize();
        g_titleButton = gui->createWidget<MyGUI::Button>("Kenshi_Button1",
            MyGUI::IntCoord((int)(view.width * 0.261f), (int)(view.height * 0.895f), (int)(view.width * 0.155f), (int)(view.height * 0.052f)),
            MyGUI::Align::Default, "Main", "KenshiMP_TitleButton");
        if (!g_titleButton) return;
        g_titleButton->setCaption(std::string(T("MULTIPLAYER")) + " (" + g_cfg.lobbyKeyName + ")");
        g_titleButton->eventMouseButtonClick += MyGUI::newDelegate(onTitleButton);
    }
    catch (...) { g_titleButton = NULL; }
}

// Pause menu (Escape): a MULTIPLAYER button right under Kenshi's own menu, same skin and width.
// The game's panel is found by name (MyGUI prefixes layout names); our button is its sibling, so
// it lives and dies with the menu and no pointer to it is kept between frames.
namespace
{
    bool endsWith(const std::string& s, const char* suffix)
    {
        size_t n = strlen(suffix);
        return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
    }
    MyGUI::Widget* findByNameSuffix(MyGUI::Widget* w, const char* suffix, int depth)
    {
        if (!w || depth > 12) return NULL;
        if (endsWith(w->getName(), suffix)) return w;
        for (size_t i = 0; i < w->getChildCount(); ++i)
            if (MyGUI::Widget* f = findByNameSuffix(w->getChildAt(i), suffix, depth + 1)) return f;
        return NULL;
    }
    void onPauseButton(MyGUI::Widget*) { if (!g_win) create(); }
    void listButtons(MyGUI::Widget* w, std::string& out, int depth)
    {
        if (!w || depth > 12 || out.size() > 1500) return;
        const std::string& n = w->getName();
        if (n.find("Button") != std::string::npos && w->getInheritedVisible()) { out += n; out += ' '; }
        for (size_t i = 0; i < w->getChildCount(); ++i) listButtons(w->getChildAt(i), out, depth + 1);
    }
}

// ---------------------------------------------------------------- the game's Factions screen
// When another player's faction is selected there, the same WAR / PEACE / ALLY / TRADE buttons
// as in the Multiplayer window appear under its description (same handlers).
namespace
{
    MyGUI::Widget* g_facBar = NULL;
    MyGUI::Widget* g_facParent = NULL;
    std::string g_facShown;

    FactionsScreen* factionsScreen()
    {
        ManagementScreen* ms = ManagementScreen::getSingleton();
        return ms ? ms->factionScreen : NULL;
    }
    void hideFactionBar() { if (g_facBar) g_facBar->setVisible(false); }
}

void lobby_factionsTick()
{
    try
    {
        FactionsScreen* fs = factionsScreen();
        if (!fs || !fs->mainWidget || !fs->getVisible() || !fs->selectedFaction || !ready()) { hideFactionBar(); return; }
        int player = -1;
        std::vector<PlayerInfo> ps = g_session.players();
        for (size_t i = 0; i < ps.size() && player < 0; ++i)
            if (ps[i].id != g_session.localId() && factionFor(ps[i].id) == fs->selectedFaction) player = ps[i].id;
        if (player < 0) { hideFactionBar(); return; }
        float rel = diplomacy_relation((uint8_t)player);
        char sig[64]; sprintf_s(sig, "%d:%.0f:%d", player, rel, (int)trade_pendingFrom((uint8_t)player));
        if (g_facBar && g_facParent != fs->mainWidget) { g_facBar = NULL; g_facShown.clear(); }   // the screen was rebuilt (its widgets with it)
        if (!g_facBar || g_facShown != sig)
        {
            if (g_facBar) MyGUI::Gui::getInstance().destroyWidget(g_facBar);
            MyGUI::Widget* parent = fs->mainWidget;
            int w = parent->getWidth(), h = parent->getHeight(), bw = 92, gap = 6, bh = 30;
            g_facBar = parent->createWidget<MyGUI::Widget>("", MyGUI::IntCoord(16, h - bh - 16, w - 32, bh), MyGUI::Align::Left | MyGUI::Align::Bottom, "KenshiMP_FactionBar");
            g_facParent = parent;
            g_facShown = sig;
            const char* state = rel < 0 ? T("at war") : rel > 50 ? T("allied") : T("neutral");
            MyGUI::TextBox* label = g_facBar->createWidget<MyGUI::TextBox>("Kenshi_TextboxStandardText",
                MyGUI::IntCoord(0, 0, g_facBar->getWidth() - 4 * (bw + gap), bh), MyGUI::Align::Default);
            label->setCaption(MyGUI::UString(std::string("KenshiMP : ") + state));
            const char* caps[4] = { "War", "Peace", "Ally", trade_pendingFrom((uint8_t)player) ? "Accept" : "Trade" };
            const char kinds[4] = { 'w', 'p', 'a', 't' };
            for (int k = 0; k < 4; ++k)
            {
                char name[48]; sprintf_s(name, "KMP_Dip_%d_%c", player, kinds[k]);   // same handler as the Multiplayer window
                MyGUI::Button* b = g_facBar->createWidget<MyGUI::Button>("Kenshi_Button1",
                    MyGUI::IntCoord(g_facBar->getWidth() - (4 - k) * (bw + gap), 0, bw, bh), MyGUI::Align::Default, name);
                b->setCaption(T(caps[k]));
                b->eventMouseButtonClick += MyGUI::newDelegate(onButton);
            }
            log("factions screen: diplomacy buttons shown for %s (%s)", playerName((uint8_t)player).c_str(), state);
        }
        g_facBar->setVisible(true);
    }
    catch (...) { g_facBar = NULL; g_facShown.clear(); }
}

// Test runs: open the Factions screen on that player's faction (no click needed).
bool lobby_debugOpenFactions(uint8_t player, bool open)
{
    ManagementScreen* ms = ManagementScreen::getSingleton();
    FactionsScreen* fs = factionsScreen();
    if (!ms || !fs) return false;
    ms->setVisible(open, 1);   // tab 1 = FACTIONS
    if (open) fs->selectedFaction = factionFor(player);
    return true;
}

// Test runs: press a visible title screen button whose widget name ends with `suffix` (as a click
// would). Returns false while it is not there; logs the button names once to help find them.
bool lobby_pressTitleButton(const char* suffix)
{
    try
    {
        MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
        if (!gui) return false;
        MyGUI::Widget* b = NULL;
        MyGUI::EnumeratorWidgetPtr roots = gui->getEnumerator();
        while (!b && roots.next()) b = findByNameSuffix(roots.current(), suffix, 0);
        if (!b || !b->getInheritedVisible())
        {
            static bool listed = false;
            if (!listed)
            {
                listed = true;
                std::string names;
                MyGUI::EnumeratorWidgetPtr all = gui->getEnumerator();
                while (all.next()) listButtons(all.current(), names, 0);
                log("title buttons: %s", names.c_str());
            }
            return false;
        }
        b->eventMouseButtonClick(b);
        return true;
    }
    catch (...) { return false; }
}

// The Escape that closed our window also reached the game, which opened its pause menu: a native
// window would have consumed it. Resume right away (as if CONTINUE had been clicked).
static void swallowEscapePause()
{
    if (!g_escapeClosedAt) return;
    if (GetTickCount() - g_escapeClosedAt > 600) { g_escapeClosedAt = 0; return; }
    try
    {
        MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
        if (!gui) return;
        MyGUI::Widget* resume = NULL;
        MyGUI::EnumeratorWidgetPtr roots = gui->getEnumerator();
        while (!resume && roots.next()) resume = findByNameSuffix(roots.current(), "ResumeButton", 0);
        if (!resume || !resume->getInheritedVisible()) return;
        g_escapeClosedAt = 0;
        resume->eventMouseButtonClick(resume);
    }
    catch (...) { g_escapeClosedAt = 0; }
}

void lobby_pauseButton()
{
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 300) return;
    last = now;
    try
    {
        MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
        if (!gui) return;
        MyGUI::Widget* panel = NULL;
        MyGUI::EnumeratorWidgetPtr roots = gui->getEnumerator();
        while (!panel && roots.next()) panel = findByNameSuffix(roots.current(), "MainMenuPopupPanel", 0);
        if (!panel || !panel->getInheritedVisible()) return;
        MyGUI::Widget* parent = panel->getParent();
        if (!parent) return;
        for (size_t i = 0; i < parent->getChildCount(); ++i)
            if (parent->getChildAt(i)->getName() == "KenshiMP_PauseButton") return;   // already there
        const MyGUI::IntCoord& pc = panel->getCoord();
        int w = (int)(pc.width * 0.75f), h = (int)(pc.height * 0.13f);
        MyGUI::Button* b = parent->createWidget<MyGUI::Button>("Kenshi_Button1",
            MyGUI::IntCoord(pc.left + (pc.width - w) / 2, pc.top + pc.height + 8, w, h), MyGUI::Align::Default, "KenshiMP_PauseButton");
        b->setCaption(T("MULTIPLAYER"));
        b->eventMouseButtonClick += MyGUI::newDelegate(onPauseButton);
    }
    catch (...) {}
}

void lobby_toggle()
{
    if (g_win) { g_action = CLOSE; return; }
    if (!create()) log("lobby window could not be created");
}

bool lobby_isOpen() { return g_win != NULL; }

void lobby_setError(const std::string& e) { g_lastError = e; g_lastStatus = 0; }

void lobby_tick()
{
    Action a = g_action;
    g_action = NONE;
    switch (a)
    {
    case HOST:
    case JOIN:
        if (!readFields()) break;
        g_lastError.clear();
        mp_start(a == HOST ? "host" : "join");
        break;
    case LEAVE:
        g_lastError.clear();
        mp_leave();
        break;
    case CLOSE:
        destroy();
        return;
    default:
        break;
    }
    swallowEscapePause();
    if (g_inviteId)
    {
        unsigned long long who = g_inviteId; g_inviteId = 0;
        if (!g_session.active() || !g_session.isHost()) mp_start("host");   // you invite: you host
        showMessage(steam_invite(who) ? T("Steam invitation sent.") : T("Steam invitation failed."));
    }
    if (g_dipPlayer >= 0)
    {
        int p = g_dipPlayer; g_dipPlayer = -1;
        if (ready())
        {
            if (g_dipValue > 1500.f) { showMessage(chars_regroupTo((uint8_t)p)); destroy(); return; }   // go to: see where we land
            if (g_dipValue > 500.f) { trade_request((uint8_t)p); destroy(); return; }   // trade: our window makes room for the game's
            diplomacy_set(p, g_dipValue);
        }
    }
    if (g_win && GetTickCount() - g_lastStatus > 500) { g_lastStatus = GetTickCount(); refreshStatus(); refreshDiplomacy(); }
}


} // namespace kmp
