// Interface language: the mod speaks the game's language (Kenshi's settings.cfg, "language=fr_FR"),
// or the one forced by kenshimp.cfg (language=fr|en). Texts are written in English in the code and
// looked up here; anything missing simply stays in English.
// NOTE: this file is UTF-8 without BOM on purpose (VC2010 then keeps the bytes of narrow literals,
// and Kenshi/MyGUI expect UTF-8).
#include "Shared.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <map>

namespace kmp {

namespace
{
    bool g_french = false;

    struct Pair { const char* en; const char* fr; };
    const Pair FR[] =
    {
        // --- lobby window
        { "Multiplayer", "Multijoueur" },
        { "Name", "Nom" },
        { "Faction", "Faction" },
        { "Host address", "Adresse de l'hôte" },
        { "Port", "Port" },
        { "Password", "Mot de passe" },
        { "Host", "Héberger" },
        { "Join", "Rejoindre" },
        { "Leave", "Quitter" },
        { "Close", "Fermer" },
        { "MULTIPLAYER", "MULTIJOUEUR" },
        { "Enter a name.", "Entrez un nom." },
        { "Port must be between 1 and 65535.", "Le port doit être compris entre 1 et 65535." },
        { "Not hosting yet: press Host (or load a game, kenshimp.cfg says mode=host).",
          "Pas encore hébergé : cliquez sur Héberger (ou chargez une partie, kenshimp.cfg indique mode=host)." },
        { "Not connected. Press Join (retries every 10 s once in game).",
          "Non connecté. Cliquez sur Rejoindre (nouvel essai toutes les 10 s une fois en jeu)." },
        { "Not connected. Host a game, or enter the host's address and press Join.",
          "Non connecté. Hébergez une partie, ou entrez l'adresse de l'hôte et cliquez sur Rejoindre." },
        { "Hosting on port %d.", "Partie hébergée sur le port %d." },
        { " This PC: %s.", " Ce PC : %s." },
        { "Friends on your network/VPN (Radmin, ZeroTier, Tailscale) join with one of these addresses; over the Internet use your public IP and forward the TCP port on your router.",
          "Vos amis sur le même réseau ou VPN (Radmin, ZeroTier, Tailscale) rejoignent avec l'une de ces adresses ; par Internet, donnez votre IP publique et redirigez le port TCP sur votre box." },
        { "Start or load a game: players join your world.", "Lancez ou chargez une partie : les joueurs rejoignent votre monde." },
        { "Connecting to %s... (the connection completes once a game is loaded)",
          "Connexion à %s... (elle se termine une fois une partie chargée)" },
        { "Connected to %s:%d - ping %d ms.", "Connecté à %s:%d - ping %d ms." },
        { "Players:", "Joueurs :" },
        { "  <- you", "  <- vous" },
        { "%s opens/closes this window. Enter = chat.", "%s ouvre/ferme cette fenêtre. Entrée = discussion." },
        // --- session events
        { "Connected to the multiplayer session.", "Connecté à la partie multijoueur." },
        { "Disconnected: %s", "Déconnecté : %s" },
        { "Warning: %s", "Avertissement : %s" },
        { "Multiplayer disconnected: %s", "Multijoueur déconnecté : %s" },
        { " - reconnecting automatically...", " - reconnexion automatique..." },
        { "Multiplayer warning: %s", "Avertissement multijoueur : %s" },
        { "%s joined the game", "%s a rejoint la partie" },
        { "The world changed: %s (host's world).", "Le monde a changé : %s (monde de l'hôte)." },
        { "%d towns changed to match the host's world.", "%d villes ont changé pour suivre le monde de l'hôte." },
        { "%s: bounty from %s (+%d), seen in the host's world.", "%s : prime de %s (+%d), vu dans le monde de l'hôte." },
        { "%s left the game", "%s a quitté la partie" },
        { "Multiplayer: %s", "Multijoueur : %s" },
        { "KenshiMP: %s = multiplayer window (host / join), Enter = chat.",
          "KenshiMP : %s = fenêtre multijoueur (héberger / rejoindre), Entrée = discussion." },
        { "Multiplayer: you left the session.", "Multijoueur : vous avez quitté la partie." },
        { "Multiplayer: saved as '%s' (your solo save is kept intact).",
          "Multijoueur : sauvegardé sous « %s » (votre sauvegarde solo reste intacte)." },
        { "Pause is controlled by the host in multiplayer.", "En multijoueur, seul l'hôte peut mettre en pause." },
        // --- diplomacy
        { "You declared war on %s.", "Vous avez déclaré la guerre à %s." },
        { "You allied with %s.", "Vous vous êtes allié à %s." },
        { "You made peace with %s.", "Vous avez fait la paix avec %s." },
        { "the other players", "les autres joueurs" },
        { "%s declared war on you!", "%s vous a déclaré la guerre !" },
        { "%s allied with you.", "%s s'est allié à vous." },
        { "%s made peace with you.", "%s a fait la paix avec vous." },
        // --- rules
        { "Cannot build there: another player's building is in the way.",
          "Impossible de construire ici : un bâtiment d'un autre joueur gêne." },
        { "You cannot recruit another player's character.", "Vous ne pouvez pas recruter le personnage d'un autre joueur." },
        { "You cannot carry or cage another player's character.", "Vous ne pouvez pas porter ni encager le personnage d'un autre joueur." },
        { "%s took %s from your squad.", "%s a pris %s à votre escouade." },
        { "%s recruited an NPC.", "%s a recruté un PNJ." },
        { "%s's character %s (speed hack or lag?).", "Le personnage de %s %s (triche ou décalage ?)." },
        { "jumped %.0f units", "a sauté de %.0f unités" },
        { "moved at %.0f units/s", "s'est déplacé à %.0f unités/s" },
        // --- chat
        { "Players: %s", "Joueurs : %s" },
        { " (you)", " (vous)" },
        { "No player named '%s'. Type /players.", "Aucun joueur nommé « %s ». Tapez /joueurs." },
        { "Several players match '%s', type more of the name.", "Plusieurs joueurs correspondent à « %s », tapez plus de lettres." },
        { "Commands: /players, /war <player|all>, /peace <player|all>, /ally <player|all>",
          "Commandes : /joueurs, /guerre <joueur|tous>, /paix <joueur|tous>, /allie <joueur|tous>" },
        { "Slow down.", "Doucement." },
        { "War", "Guerre" },
        { "Peace", "Paix" },
        { "Ally", "Allié" },
        { "at war", "en guerre" },
        { "allied", "allié" },
        { "neutral", "neutre" },
        { "Trade", "Échanger" },
        { "Steam friends", "Amis Steam" },
        { "Invite", "Inviter" },
        { "in Kenshi", "dans Kenshi" },
        { "No Steam friend online.", "Aucun ami Steam en ligne." },
        { "Steam invitation sent.", "Invitation Steam envoyée." },
        { "Steam invitation failed.", "L'invitation Steam a échoué." },
        { "Joining your friend's game through Steam...", "Connexion à la partie de votre ami par Steam..." },
        { "Steam: %s. ", "Steam : %s. " },
        { "Steam friends can join you: STEAM FRIENDS > INVITE, or \"Join game\" on your profile.", "Vos amis Steam peuvent vous rejoindre : AMIS STEAM > INVITER, ou « Rejoindre la partie » sur votre profil." },
        { "Accept a friend's Steam invitation to join them (no address needed).", "Acceptez l'invitation Steam d'un ami pour le rejoindre (pas besoin d'adresse)." },
        { "Steam is not available (start Kenshi from Steam) or the host could not be reached", "Steam n'est pas disponible (lancez Kenshi depuis Steam) ou l'hôte est injoignable" },
        { "%s attacked your squad: you are now at war.", "%s a attaqué votre escouade : vous êtes maintenant en guerre." },
        { "%s attacked your squad (relation %.0f).", "%s a attaqué votre escouade (relation %.0f)." },
        { "%s refused the payment (too far from the trader?).", "%s a refusé le paiement (trop loin du marchand ?)." },
        { "%s kept %s (someone was faster, or you were too far).", "%s garde %s (quelqu'un a été plus rapide, ou vous étiez trop loin)." },
        { "%s could not take %s: it is back in your inventory.", "%s n'a pas pu prendre %s : l'objet est revenu dans votre inventaire." },
        { "Accept", "Accepter" },
        { "Walk up to one of %s's characters to trade.", "Approchez-vous d'un personnage de %s pour échanger." },
        { "Trade offer sent to %s.", "Proposition d'échange envoyée à %s." },
        { "%s offers to trade: F4, then ACCEPT on their line.", "%s propose un échange : F4, puis ACCEPTER sur sa ligne." },
        { "%s offers to trade.", "%s propose un échange." },
        { "%s accepted the trade.", "%s a accepté l'échange." },
        { "%s ended the trade.", "%s a terminé l'échange." },
        { "Connecting to the host...", "Connexion à l'hôte..." },
        // --- refusals / network reasons (sent in English by the other side: translated on display)
        { "Your mods must match the host's.", "Vos mods doivent être ceux de l'hôte." },
        { "mod lists differ", "listes de mods différentes" },
        { "Missing (enable them): ", "Manquants (à activer) : " },
        { "Not on the host (disable them): ", "Absents chez l'hôte (à désactiver) : " },
        { "Same mods but a different load order: copy the host's order.", "Mêmes mods mais ordre de chargement différent : reprenez l'ordre de l'hôte." },
        { "wrong session password", "mauvais mot de passe" },
        { "server full", "partie pleine" },
        { "KenshiMP version mismatch", "version de KenshiMP différente" },
        { "timed out (no data for 15 s)", "délai dépassé (aucune donnée depuis 15 s)" },
        { "connection closed", "connexion fermée" },
        { "too slow (send backlog overflow)", "connexion trop lente" },
        { "timed out / refused (host not started, wrong IP, or port closed on the host's router/firewall?)",
          "délai dépassé ou refusée (hôte pas lancé, mauvaise IP, ou port fermé sur la box/le pare-feu de l'hôte ?)" },
        { " - port already used by another program or game?", " - port déjà utilisé par un autre programme ou jeu ?" },
        { "connection to ", "connexion à " },
        { "install the same mod version", "installez la même version du mod" },
        { "host protocol", "protocole de l'hôte" },
        { "yours", "le vôtre" },
    };

    std::map<std::string, const char*>& table()
    {
        static std::map<std::string, const char*> t;
        if (t.empty()) for (size_t i = 0; i < sizeof(FR) / sizeof(FR[0]); ++i) t[FR[i].en] = FR[i].fr;
        return t;
    }

    std::string trim(const std::string& s)
    {
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }
}

void lang_init()
{
    std::string want = g_cfg.language;
    if (want.empty() || want == "auto")
    {
        want = "en";
        FILE* f = NULL;
        if (fopen_s(&f, "settings.cfg", "r") == 0 && f)
        {
            char line[256];
            while (fgets(line, sizeof(line), f))
            {
                std::string l = trim(line);
                if (l.compare(0, 9, "language=") == 0) { want = l.substr(9); break; }
            }
            fclose(f);
        }
    }
    g_french = want.compare(0, 2, "fr") == 0;
    log("interface language: %s", g_french ? "french" : "english");
}

bool lang_french() { return g_french; }

const char* T(const char* en)
{
    if (!g_french || !en) return en;
    std::map<std::string, const char*>::iterator it = table().find(en);
    return it == table().end() ? en : it->second;
}

std::string TF(const char* en, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, en);
    vsnprintf_s(buf, sizeof(buf), _TRUNCATE, T(en), ap);
    va_end(ap);
    return buf;
}

// Network reasons are built in English by the session layer (possibly on the other machine):
// translate the known fragments in place.
std::string lang_reason(const std::string& text)
{
    if (!g_french) return text;
    std::string out = text;
    for (size_t i = 0; i < sizeof(FR) / sizeof(FR[0]); ++i)
    {
        size_t len = strlen(FR[i].en);
        if (len < 6) continue;   // whole sentences and long fragments only
        size_t p;
        while ((p = out.find(FR[i].en)) != std::string::npos) out.replace(p, len, FR[i].fr);
    }
    return out;
}

} // namespace kmp
