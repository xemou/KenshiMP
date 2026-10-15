// Server list (DEDICATED_SERVER_PLAN.md, phase 6): the text the list service (tools/server_list/list_server.py) returns,
// one server per line: address|port|transport|players|max|version|name
#pragma once
#include "Protocol.h"
#include <stdlib.h>

namespace mp {

struct ServerEntry
{
    std::string address, name, transport;
    int port, players, maxPlayers;
    uint32_t version;
    // filled by the game after asking the server itself (Session::queryInfo)
    int pingMs;               // -1 = no answer (yet)
    ServerInfo info;
    ServerEntry() : port(0), players(0), maxPlayers(0), version(0), pingMs(-1) {}
    bool compatible() const { return version == PROTOCOL_VERSION; }
};

inline std::vector<ServerEntry> parseServerList(const std::string& text)
{
    std::vector<ServerEntry> out;
    size_t a = 0;
    while (a < text.size() && out.size() < 500)
    {
        size_t b = text.find('\n', a); if (b == std::string::npos) b = text.size();
        std::string line = text.substr(a, b - a);
        a = b + 1;
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        std::vector<std::string> f;
        size_t p = 0;
        for (int i = 0; i < 6; ++i)
        {
            size_t q = line.find('|', p);
            if (q == std::string::npos) break;
            f.push_back(line.substr(p, q - p));
            p = q + 1;
        }
        if (f.size() != 6) continue;
        ServerEntry e;
        e.address = f[0]; e.port = atoi(f[1].c_str()); e.transport = f[2] == "udp" ? "udp" : "tcp";
        e.players = atoi(f[3].c_str()); e.maxPlayers = atoi(f[4].c_str()); e.version = (uint32_t)strtoul(f[5].c_str(), NULL, 10);
        e.name = line.substr(p, 60);
        if (e.address.empty() || e.port <= 0 || e.port > 65535) continue;
        out.push_back(e);
    }
    return out;
}

} // namespace mp
