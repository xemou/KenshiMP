#include "Tunnel.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <map>
#include <vector>

namespace mp {

namespace
{
    struct Stream
    {
        SOCKET s;
        Bytes out;           // bytes waiting to be written to the local socket
        Stream() : s(INVALID_SOCKET) {}
    };

    void nonBlocking(SOCKET s) { u_long one = 1; ioctlsocket(s, FIONBIO, &one); }

    SOCKET connectLocal(int port)
    {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return s;
        sockaddr_in a; memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s, (sockaddr*)&a, sizeof(a)) != 0) { closesocket(s); return INVALID_SOCKET; }
        nonBlocking(s);
        BOOL nd = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof(nd));
        return s;
    }
}

class TunnelImpl
{
public:
    bool isHost;
    P2PTransport* transport;
    int sessionPort;          // host: the session's TCP port
    uint64_t hostPeer;        // client: who we talk to
    SOCKET listenSock;        // client: where the session connects
    int listenPort;
    std::map<uint64_t, Stream> streams;
    volatile bool running, hostClosed;
    volatile long peerCount;
    HANDLE thread;
    bool wsa;

    TunnelImpl(bool host) : isHost(host), transport(NULL), sessionPort(0), hostPeer(0), listenSock(INVALID_SOCKET),
                            listenPort(0), running(false), hostClosed(false), peerCount(0), thread(NULL), wsa(false) {}
    ~TunnelImpl() { stop(); }

    bool startWsa(std::string& err)
    {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) { err = "WSAStartup failed"; return false; }
        wsa = true;
        return true;
    }

    bool startHost(P2PTransport* t, int port, std::string& err)
    {
        if (!t) { err = "no transport"; return false; }
        if (!startWsa(err)) return false;
        transport = t; sessionPort = port;
        return launch();
    }

    bool startClient(P2PTransport* t, uint64_t peer, std::string& err)
    {
        if (!t || !peer) { err = "no transport / host"; return false; }
        if (!startWsa(err)) return false;
        transport = t; hostPeer = peer;
        listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a; memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET; a.sin_port = 0; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (listenSock == INVALID_SOCKET || bind(listenSock, (sockaddr*)&a, sizeof(a)) != 0 || listen(listenSock, 4) != 0)
        { err = "tunnel: cannot listen on 127.0.0.1"; stop(); return false; }
        int len = sizeof(a);
        getsockname(listenSock, (sockaddr*)&a, &len);
        listenPort = ntohs(a.sin_port);
        nonBlocking(listenSock);
        return launch();
    }

    bool launch()
    {
        running = true;
        thread = CreateThread(NULL, 0, &TunnelImpl::threadMain, this, 0, NULL);
        return thread != NULL;
    }
    static DWORD WINAPI threadMain(LPVOID p) { ((TunnelImpl*)p)->loop(); return 0; }

    void stop()
    {
        running = false;
        if (thread) { WaitForSingleObject(thread, 3000); CloseHandle(thread); thread = NULL; }
        for (std::map<uint64_t, Stream>::iterator it = streams.begin(); it != streams.end(); ++it)
        {
            if (transport) sendKind(it->first, TUNNEL_BYE, NULL, 0);
            if (it->second.s != INVALID_SOCKET) closesocket(it->second.s);
        }
        streams.clear();
        peerCount = 0;
        if (listenSock != INVALID_SOCKET) { closesocket(listenSock); listenSock = INVALID_SOCKET; }
        if (wsa) { WSACleanup(); wsa = false; }
    }

    bool sendKind(uint64_t peer, uint8_t kind, const char* data, uint32_t n)
    {
        std::vector<uint8_t> m(1 + n);
        m[0] = kind;
        if (n) memcpy(&m[1], data, n);
        return transport->send(peer, &m[0], (uint32_t)m.size());
    }

    void closeStream(uint64_t peer, bool tellPeer)
    {
        std::map<uint64_t, Stream>::iterator it = streams.find(peer);
        if (it == streams.end()) return;
        if (tellPeer) sendKind(peer, TUNNEL_BYE, NULL, 0);
        if (it->second.s != INVALID_SOCKET) closesocket(it->second.s);
        streams.erase(it);
        if (isHost) transport->close(peer);
        peerCount = (long)streams.size();
    }

    // Host: a new remote stream = a new local connection to the session.
    Stream* openHostStream(uint64_t peer)
    {
        closeStream(peer, false);
        SOCKET s = connectLocal(sessionPort);
        if (s == INVALID_SOCKET) { sendKind(peer, TUNNEL_BYE, NULL, 0); return NULL; }
        Stream& st = streams[peer];
        st.s = s;
        peerCount = (long)streams.size();
        return &st;
    }

    void onMessage(uint64_t peer, const Bytes& m)
    {
        if (m.empty()) return;
        uint8_t kind = m[0];
        if (isHost)
        {
            if (kind == TUNNEL_HELLO) { openHostStream(peer); return; }
            if (kind == TUNNEL_BYE) { closeStream(peer, false); return; }
            if (kind == TUNNEL_DATA)
            {
                std::map<uint64_t, Stream>::iterator it = streams.find(peer);
                Stream* st = it != streams.end() ? &it->second : openHostStream(peer);
                if (st) st->out.insert(st->out.end(), m.begin() + 1, m.end());
            }
            return;
        }
        if (peer != hostPeer) return;   // client: only the host talks to us
        if (kind == TUNNEL_BYE) { hostClosed = true; closeStream(peer, false); return; }
        if (kind == TUNNEL_DATA)
        {
            std::map<uint64_t, Stream>::iterator it = streams.find(peer);
            if (it != streams.end()) it->second.out.insert(it->second.out.end(), m.begin() + 1, m.end());
        }
    }

    void loop()
    {
        char buf[TUNNEL_CHUNK];
        while (running)
        {
            // transport -> local sockets
            uint64_t peer; Bytes m;
            for (int n = 0; n < 512 && transport->receive(peer, m); ++n) onMessage(peer, m);

            // client: the session (re)connects to us
            if (!isHost && listenSock != INVALID_SOCKET)
            {
                SOCKET s = accept(listenSock, NULL, NULL);
                if (s != INVALID_SOCKET)
                {
                    closeStream(hostPeer, true);
                    nonBlocking(s);
                    BOOL nd = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof(nd));
                    Stream& st = streams[hostPeer];
                    st.s = s;
                    hostClosed = false;
                    peerCount = (long)streams.size();
                    sendKind(hostPeer, TUNNEL_HELLO, NULL, 0);
                }
            }

            // local sockets: read -> DATA, write what the peer sent
            fd_set rs, ws; FD_ZERO(&rs); FD_ZERO(&ws);
            int count = 0;
            for (std::map<uint64_t, Stream>::iterator it = streams.begin(); it != streams.end() && count < FD_SETSIZE - 1; ++it)
            {
                FD_SET(it->second.s, &rs);
                if (!it->second.out.empty()) FD_SET(it->second.s, &ws);
                ++count;
            }
            if (count == 0) { Sleep(2); continue; }
            timeval tv; tv.tv_sec = 0; tv.tv_usec = 2000;
            if (select(0, &rs, &ws, NULL, &tv) < 0) { Sleep(2); continue; }
            std::vector<uint64_t> dead;
            for (std::map<uint64_t, Stream>::iterator it = streams.begin(); it != streams.end(); ++it)
            {
                Stream& st = it->second;
                if (FD_ISSET(st.s, &rs))
                {
                    int r = recv(st.s, buf, sizeof(buf), 0);
                    if (r > 0) { if (!sendKind(it->first, TUNNEL_DATA, buf, (uint32_t)r)) dead.push_back(it->first); }
                    else if (r == 0 || WSAGetLastError() != WSAEWOULDBLOCK) dead.push_back(it->first);
                }
                if (!st.out.empty() && FD_ISSET(st.s, &ws))
                {
                    int w = ::send(st.s, (const char*)&st.out[0], (int)(st.out.size() > 65536 ? 65536 : st.out.size()), 0);
                    if (w > 0) st.out.erase(st.out.begin(), st.out.begin() + w);
                    else if (WSAGetLastError() != WSAEWOULDBLOCK) dead.push_back(it->first);
                }
            }
            for (size_t i = 0; i < dead.size(); ++i) closeStream(dead[i], true);
        }
    }
};

TunnelHost::TunnelHost() : impl_(new TunnelImpl(true)) {}
TunnelHost::~TunnelHost() { delete impl_; }
bool TunnelHost::start(P2PTransport* t, int port, std::string& err) { stop(); return impl_->startHost(t, port, err); }
void TunnelHost::stop() { impl_->stop(); }
bool TunnelHost::running() const { return impl_->running; }
int TunnelHost::peers() const { return (int)impl_->peerCount; }

TunnelClient::TunnelClient() : impl_(new TunnelImpl(false)) {}
TunnelClient::~TunnelClient() { delete impl_; }
bool TunnelClient::start(P2PTransport* t, uint64_t peer, std::string& err) { stop(); return impl_->startClient(t, peer, err); }
void TunnelClient::stop() { impl_->stop(); }
bool TunnelClient::running() const { return impl_->running; }
int TunnelClient::localPort() const { return impl_->listenPort; }
bool TunnelClient::hostClosed() const { return impl_->hostClosed; }

} // namespace mp
