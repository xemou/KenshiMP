// Tunnel: carries the session's TCP byte stream over a peer-to-peer message transport (Steam's
// P2P network in the game, an in-process fake in the tests), so players can join without an IP
// address, an open port or a VPN. The session itself is unchanged: it still talks TCP, to
// 127.0.0.1.
//
//   client game --TCP--> TunnelClient (127.0.0.1:localPort) ==P2P==> TunnelHost --TCP--> host session
//
// Messages on the transport: u8 kind + payload. HELLO opens the stream (client -> host), DATA
// carries bytes in order, BYE closes it (either way). The transport must be reliable and ordered.
// VS2010-compatible C++ (no C++11).
#pragma once
#include "Protocol.h"

namespace mp {

class P2PTransport
{
public:
    virtual ~P2PTransport() {}
    // Reliable, ordered message to that peer (false: could not queue it).
    virtual bool send(uint64_t peer, const uint8_t* data, uint32_t size) = 0;
    // One received message, if any.
    virtual bool receive(uint64_t& peer, Bytes& out) = 0;
    // Forget that peer (end of its stream).
    virtual void close(uint64_t peer) = 0;
};

enum { TUNNEL_HELLO = 1, TUNNEL_DATA = 2, TUNNEL_BYE = 3 };
const uint32_t TUNNEL_CHUNK = 16 * 1024;   // bytes per DATA message

class TunnelImpl;

// Host side: every peer that says HELLO gets its own TCP connection to the local session port.
class TunnelHost
{
public:
    TunnelHost();
    ~TunnelHost();
    bool start(P2PTransport* transport, int sessionPort, std::string& err);
    void stop();
    bool running() const;
    int peers() const;             // open streams
private:
    TunnelHost(const TunnelHost&);
    TunnelHost& operator=(const TunnelHost&);
    TunnelImpl* impl_;
};

// Client side: listens on 127.0.0.1 (port chosen by the system); the session joins that port and
// its bytes go to the host peer.
class TunnelClient
{
public:
    TunnelClient();
    ~TunnelClient();
    bool start(P2PTransport* transport, uint64_t hostPeer, std::string& err);
    void stop();
    bool running() const;
    int localPort() const;        // where the session must connect
    bool hostClosed() const;      // the host ended the stream (BYE)
private:
    TunnelClient(const TunnelClient&);
    TunnelClient& operator=(const TunnelClient&);
    TunnelImpl* impl_;
};

} // namespace mp
