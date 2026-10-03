// Chat: Enter opens a one-line box at the bottom of the screen (Kenshi's own edit-box skin),
// Enter sends, Escape cancels. The last lines stay above it (player names in their colour) and
// fade after a while; opening the box shows the whole recent history again.
// Commands:  /war, /peace, /ally <player|all>  (relation -100 / 0 / 100)   /players
//            /goto [player]  (our selected characters travel next to that player's; default: the host)
//            /report  (bug report folder on the desktop, see Report.cpp)
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <mygui/MyGUI.h>

#include "Shared.h"

#include <ctype.h>
#include <deque>

using namespace mp;

namespace kmp {

namespace
{
    const size_t MAX_CHAT = 200;
    const DWORD MIN_INTERVAL_MS = 400;     // anti-spam

    MyGUI::EditBox* g_box = NULL;
    bool g_closeRequested = false;          // widgets are not destroyed from their own events
    std::string g_submitted;
    bool g_hasSubmitted = false;
    DWORD g_lastSent = 0;

    std::string lower(std::string s) { for (size_t i = 0; i < s.size(); ++i) s[i] = (char)tolower((unsigned char)s[i]); return s; }

    DWORD g_openedAt = 0, g_closedAt = 0;

    // ------------------------------------------------------------------ history
    const size_t MAX_LINES = 8;
    const DWORD LINE_LIFE_MS = 20000;
    struct Line { std::string text; DWORD at; };
    std::deque<Line> g_lines;
    MyGUI::EditBox* g_log = NULL;
    std::string g_shown;
    bool g_logFailed = false;

    const char* playerColour(uint8_t id)
    {
        static const char* c[8] = { "#E8C170", "#7FB3E0", "#9AD07F", "#E08F7F", "#C59AE0", "#7FD9D0", "#E0D27F", "#D0D0D0" };
        return c[id % 8];
    }

    bool createLog()
    {
        try
        {
            MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
            if (!gui) return false;
            const MyGUI::IntSize& view = MyGUI::RenderManager::getInstance().getViewSize();
            int w = view.width / 3 < 420 ? 420 : view.width / 3;
            int h = 8 * 22 + 8;
            g_log = gui->createWidget<MyGUI::EditBox>("Kenshi_WordWrapEmpty",
                MyGUI::IntCoord(20, (int)(view.height * 0.62f) - h - 4, w, h), MyGUI::Align::Default, "Main", "KenshiMP_ChatLog");
            if (!g_log) return false;
            g_log->setEditMultiLine(true);
            g_log->setEditWordWrap(true);
            g_log->setEditReadOnly(true);
            g_log->setTextAlign(MyGUI::Align::Left | MyGUI::Align::Bottom);
            g_log->setNeedMouseFocus(false);       // never steals clicks from the game
            g_log->setNeedKeyFocus(false);
            g_log->setCaption("");
            return true;
        }
        catch (...) { g_log = NULL; return false; }
    }
    void destroyLog()
    {
        try { if (g_log) MyGUI::Gui::getInstance().destroyWidget(g_log); } catch (...) {}
        g_log = NULL; g_shown.clear();
    }
    void refreshLog(bool all)
    {
        if (!g_log && !g_logFailed && !g_lines.empty()) { if (!createLog()) { g_logFailed = true; log("chat history panel unavailable"); } }
        if (!g_log) return;
        DWORD now = GetTickCount();
        std::string text;
        for (size_t i = 0; i < g_lines.size(); ++i)
        {
            if (!all && now - g_lines[i].at > LINE_LIFE_MS) continue;
            if (!text.empty()) text += "\n";
            text += g_lines[i].text;
        }
        if (text == g_shown) return;
        g_shown = text;
        try { g_log->setCaption(text); } catch (...) {}
    }

    void onAccept(MyGUI::EditBox* box)
    {
        // The Enter that opened the box can reach it too: ignore an empty accept right away.
        if (GetTickCount() - g_openedAt < 300 && box->getOnlyText().empty()) return;
        g_submitted = box->getOnlyText().asUTF8();
        g_hasSubmitted = true;
        g_closeRequested = true;
    }
    void onKey(MyGUI::Widget*, MyGUI::KeyCode key, MyGUI::Char)
    {
        if (key == MyGUI::KeyCode::Escape) g_closeRequested = true;
    }

    bool safeOpen()
    {
        try
        {
            MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
            if (!gui) return false;
            const MyGUI::IntSize& view = MyGUI::RenderManager::getInstance().getViewSize();
            int w = view.width / 3 < 420 ? 420 : view.width / 3;
            g_box = gui->createWidget<MyGUI::EditBox>("Kenshi_EditBox", MyGUI::IntCoord(20, (int)(view.height * 0.62f), w, 34),
                                                      MyGUI::Align::Default, "Popup", "KenshiMP_Chat");
            if (!g_box) return false;
            g_box->setMaxTextLength(MAX_CHAT);
            g_box->setEditMultiLine(false);
            g_box->eventEditSelectAccept += MyGUI::newDelegate(onAccept);
            g_box->eventKeyButtonPressed += MyGUI::newDelegate(onKey);
            MyGUI::InputManager::getInstance().setKeyFocusWidget(g_box);
            g_openedAt = GetTickCount();
            return true;
        }
        catch (...) { g_box = NULL; return false; }
    }
    void safeClose()
    {
        try
        {
            if (g_box)
            {
                MyGUI::InputManager::getInstance().resetKeyFocusWidget(g_box);
                MyGUI::Gui::getInstance().destroyWidget(g_box);
            }
        }
        catch (...) {}
        if (g_box) g_closedAt = GetTickCount();
        g_box = NULL;
    }
    bool someoneTyping()
    {
        try
        {
            // Only an editable, visible text field counts: Kenshi and other mods leave the key focus
            // on windows and buttons all the time.
            MyGUI::InputManager* in = MyGUI::InputManager::getInstancePtr();
            MyGUI::Widget* w = in ? in->getKeyFocusWidget() : NULL;
            if (!w) return false;
            MyGUI::EditBox* e = w->castType<MyGUI::EditBox>(false);
            bool typing = e && e->getInheritedVisible() && !e->getEditReadOnly();
            if (typing) log("chat: not opened, '%s' has the keyboard", w->getName().c_str());
            return typing;
        }
        catch (...) { return false; }
    }

    // Returns the player id named (case-insensitive prefix), -1 for "all", -2 if unknown.
    int findPlayer(const std::string& name)
    {
        std::string n = lower(name);
        if (n.empty() || n == "all" || n == "tous") return -1;
        const std::vector<PlayerInfo>& ps = g_session.players();
        int found = -2;
        for (size_t i = 0; i < ps.size(); ++i)
        {
            if (ps[i].id == g_session.localId()) continue;
            std::string p = lower(ps[i].name);
            if (p == n) return ps[i].id;
            if (p.compare(0, n.size(), n) == 0) found = (found == -2) ? ps[i].id : -3;   // ambiguous
        }
        return found;
    }

    void command(const std::string& line)
    {
        std::string cmd = line, arg;
        size_t sp = line.find(' ');
        if (sp != std::string::npos) { cmd = line.substr(0, sp); arg = line.substr(sp + 1); }
        while (!arg.empty() && arg[0] == ' ') arg.erase(0, 1);
        cmd = lower(cmd);
        if (cmd == "/players" || cmd == "/joueurs")
        {
            const std::vector<PlayerInfo>& ps = g_session.players();
            std::string s;
            for (size_t i = 0; i < ps.size(); ++i) s += (i ? ", " : "") + ps[i].name + (ps[i].id == g_session.localId() ? T(" (you)") : "");
            showMessage(TF("Players: %s", s.c_str()));
            return;
        }
        if (cmd == "/report" || cmd == "/rapport")
        {
            std::string done = report_write();
            showMessage(done);
            chat_notice(done);
            return;
        }
        if (cmd == "/goto" || cmd == "/aller")
        {
            // No name: the host (clients), or the only other player.
            int who = findPlayer(arg);
            if (arg.empty() && !g_session.isHost()) who = HOST_ID;
            else if (arg.empty())
            {
                const std::vector<PlayerInfo>& ps = g_session.players();
                who = -2;
                for (size_t i = 0; i < ps.size(); ++i) if (ps[i].id != g_session.localId()) who = (who == -2) ? ps[i].id : -3;
            }
            if (who == -2) { showMessage(TF("No player named '%s'. Type /players.", arg.c_str())); return; }
            if (who < 0) { showMessage(T("Type the name of the player to join: /goto <player>.")); return; }
            showMessage(chars_regroupTo((uint8_t)who));
            return;
        }
        float value = 1.0e9f;
        if (cmd == "/war" || cmd == "/guerre") value = -100.f;
        else if (cmd == "/peace" || cmd == "/paix") value = 0.f;
        else if (cmd == "/ally" || cmd == "/allie") value = 100.f;
        if (value < 1.0e8f)
        {
            int who = findPlayer(arg);
            if (who == -2) { showMessage(TF("No player named '%s'. Type /players.", arg.c_str())); return; }
            if (who == -3) { showMessage(TF("Several players match '%s', type more of the name.", arg.c_str())); return; }
            diplomacy_set(who, value);
            return;
        }
        showMessage(T("Commands: /players, /war <player|all>, /peace <player|all>, /ally <player|all>, /goto [player], /report"));
    }
}

const char* chat_playerColour(uint8_t id) { return playerColour(id); }

std::string chat_clean(const std::string& in)
{
    std::string out;
    for (size_t i = 0; i < in.size() && out.size() < MAX_CHAT; ++i)
    {
        unsigned char c = (unsigned char)in[i];
        if (c < 32 || c == 127) continue;        // control characters
        if (c == '#') out += '#';                 // MyGUI colour codes
        out += (char)c;
    }
    return out;
}

void chat_tick()
{
    if (g_closeRequested) { g_closeRequested = false; safeClose(); }
    if (g_hasSubmitted)
    {
        g_hasSubmitted = false;
        std::string text = g_submitted;
        while (!text.empty() && text[text.size() - 1] == ' ') text.erase(text.size() - 1);
        if (text.empty()) return;
        if (text[0] == '/') { command(text); return; }
        DWORD now = GetTickCount();
        if (now - g_lastSent < MIN_INTERVAL_MS) { showMessage(T("Slow down.")); return; }
        g_lastSent = now;
        if (text.size() > MAX_CHAT) text.resize(MAX_CHAT);
        ByteWriter w; w.str(text);
        g_session.send(MSG_CHAT, w.data);
        chat_received(g_session.localId(), chat_clean(text));
    }
    static DWORD lastRefresh = 0;
    if (GetTickCount() - lastRefresh > 250) { lastRefresh = GetTickCount(); refreshLog(g_box != NULL); }
}

void chat_notice(const std::string& text)
{
    Line l;
    l.text = "#A0A0A0" + chat_clean(text);
    l.at = GetTickCount();
    g_lines.push_back(l);
    while (g_lines.size() > MAX_LINES) g_lines.pop_front();
}

void chat_received(uint8_t from, const std::string& cleanText)
{
    Line l;
    l.text = std::string(playerColour(from)) + chat_clean(playerName(from)) + "#E6E6E6: " + cleanText;
    l.at = GetTickCount();
    g_lines.push_back(l);
    while (g_lines.size() > MAX_LINES) g_lines.pop_front();
    if (g_logFailed) showMessage(playerName(from) + ": " + cleanText);   // no panel: the game's message bar
}

void chat_open()
{
    // The Enter that sent a line must not reopen the box.
    if (g_box || !g_session.active() || GetTickCount() - g_closedAt < 500 || someoneTyping()) return;
    if (!safeOpen()) log("chat box could not be created");
    else log("chat box opened");
}

bool chat_isOpen() { return g_box != NULL; }

bool chat_mayOpen()
{
    if (!someoneTyping()) return true;
    return false;
}

void chat_onWorldReload() { safeClose(); destroyLog(); g_closeRequested = false; g_hasSubmitted = false; g_logFailed = false; }

} // namespace kmp
