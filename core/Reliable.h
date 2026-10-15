// Reliability layer of the UDP transport (DEDICATED_SERVER_PLAN.md, phase 2). Pure logic, no sockets: the session
// feeds it received datagrams and sends what poll() returns. Unit-tested in tests/net_test.cpp with simulated loss.
//
// Two channels per peer:
//   - reliable, ordered: a byte stream (the same [u32 length][payload] frames as over TCP), cut into numbered
//     segments, acknowledged (cumulative + 32-bit selective), resent after a timeout. Connection, chat, inventories,
//     events, appearance and world blobs travel here;
//   - unreliable, sequenced: whole messages (entity states), fragmented if larger than one datagram, never resent;
//     an incomplete or too old message is simply dropped: the next state replaces it.
//
// Datagram: [u8 kind][u32 ack][u32 ackBits] then
//   kind DGRAM_RELIABLE   : u32 seq, segment bytes
//   kind DGRAM_UNRELIABLE : u16 useq, u8 fragIndex, u8 fragCount, fragment bytes
//   kind DGRAM_ACK        : (nothing)
// ack = next reliable segment expected from the peer; bit i of ackBits = segment ack+1+i was received too.
#pragma once
#include "Buffer.h"
#include <map>
#include <deque>

namespace mp {

enum { DGRAM_RELIABLE = 1, DGRAM_UNRELIABLE = 2, DGRAM_ACK = 3 };
const size_t   UDP_SEGMENT     = 1150;     // payload bytes per datagram (fits a 1280-byte path MTU with headers)
const size_t   UDP_MAX_INFLIGHT = 768;     // unacknowledged reliable segments (~860 KB)
const size_t   UDP_MAX_FRAGS   = 200;      // unreliable message up to ~225 KB
const unsigned UDP_MIN_RTO     = 120, UDP_MAX_RTO = 1500;

class ReliableLink
{
public:
    ReliableLink() : nextSeq_(0), nextExpected_(0), ackPending_(false), srtt_(150), useq_(0), lastDelivered_(0), anyDelivered_(false),
                     resent_(0), lost_(0) {}

    // ---- sending ----
    // Reliable bytes: appended to the stream, segmented by poll(). Returns how many bytes it accepted (the window may be
    // full: the caller keeps the rest and offers it again later).
    size_t writeStream(const uint8_t* p, size_t n)
    {
        size_t room = UDP_MAX_INFLIGHT * UDP_SEGMENT;
        size_t used = inflight_.size() * UDP_SEGMENT + pendingStream_.size();
        if (used >= room) return 0;
        size_t take = n < room - used ? n : room - used;
        pendingStream_.insert(pendingStream_.end(), p, p + take);
        return take;
    }
    // One unreliable message (fragmented when needed).
    void sendUnreliable(const Bytes& msg, std::vector<Bytes>& out)
    {
        size_t frags = (msg.size() + UDP_SEGMENT - 1) / UDP_SEGMENT;
        if (frags == 0) frags = 1;
        if (frags > UDP_MAX_FRAGS) return;
        uint16_t seq = ++useq_;
        for (size_t f = 0; f < frags; ++f)
        {
            ByteWriter w; header(w, DGRAM_UNRELIABLE);
            w.u16(seq); w.u8((uint8_t)f); w.u8((uint8_t)frags);
            size_t a = f * UDP_SEGMENT, b = a + UDP_SEGMENT < msg.size() ? a + UDP_SEGMENT : msg.size();
            if (b > a) w.raw(&msg[a], b - a);
            out.push_back(w.data);
        }
        ackPending_ = false;   // the header carried our acks
    }
    // Datagrams due now: new segments, timed-out resends, a bare ack if nothing else carried it.
    void poll(unsigned now, std::vector<Bytes>& out)
    {
        while (!pendingStream_.empty() && inflight_.size() < UDP_MAX_INFLIGHT)
        {
            size_t n = pendingStream_.size() < UDP_SEGMENT ? pendingStream_.size() : UDP_SEGMENT;
            Segment s; s.data.assign(pendingStream_.begin(), pendingStream_.begin() + n); s.sentAt = now; s.firstSent = now; s.tries = 1;
            pendingStream_.erase(pendingStream_.begin(), pendingStream_.begin() + n);
            uint32_t seq = nextSeq_++;
            inflight_[seq] = s;
            out.push_back(segmentDatagram(seq, s.data));
        }
        unsigned rto = rto_();
        int resentNow = 0;
        for (std::map<uint32_t, Segment>::iterator it = inflight_.begin(); it != inflight_.end() && resentNow < 64; ++it)
        {
            Segment& s = it->second;
            unsigned wait = rto << (s.tries > 4 ? 4 : s.tries - 1);   // back off
            if (wait > UDP_MAX_RTO * 2) wait = UDP_MAX_RTO * 2;
            if (now - s.sentAt >= wait || s.nacks >= 3)
            {
                s.sentAt = now; ++s.tries; s.nacks = 0; ++resent_; ++resentNow;
                out.push_back(segmentDatagram(it->first, s.data));
            }
        }
        if (ackPending_)
        {
            ByteWriter w; header(w, DGRAM_ACK);
            out.push_back(w.data);
        }
    }
    bool idle() const { return inflight_.empty() && pendingStream_.empty(); }
    size_t backlog() const { return inflight_.size() * UDP_SEGMENT + pendingStream_.size(); }

    // ---- receiving ----
    // stream: in-order reliable bytes; messages: complete unreliable messages. False = malformed datagram.
    bool receive(const uint8_t* p, size_t n, unsigned now, Bytes& stream, std::vector<Bytes>& messages)
    {
        ByteReader r(p, n);
        uint8_t kind = r.u8(); uint32_t ack = r.u32(); uint32_t bits = r.u32();
        if (!r.ok() || kind < DGRAM_RELIABLE || kind > DGRAM_ACK) return false;
        onAck(ack, bits, now);
        if (kind == DGRAM_RELIABLE)
        {
            uint32_t seq = r.u32();
            if (!r.ok()) return false;
            size_t off = n - r.remaining();
            ackPending_ = true;
            if ((int32_t)(seq - nextExpected_) < 0) return true;              // duplicate
            if (seq - nextExpected_ > 4096) return true;                       // far ahead: ignore, it will come again
            if (!received_.count(seq)) received_[seq] = Bytes(p + off, p + n);
            while (true)
            {
                std::map<uint32_t, Bytes>::iterator it = received_.find(nextExpected_);
                if (it == received_.end()) break;
                stream.insert(stream.end(), it->second.begin(), it->second.end());
                received_.erase(it);
                ++nextExpected_;
            }
        }
        else if (kind == DGRAM_UNRELIABLE)
        {
            uint16_t seq = r.u16(); uint8_t idx = r.u8(); uint8_t count = r.u8();
            if (!r.ok() || count == 0 || idx >= count || count > UDP_MAX_FRAGS) return false;
            // Older than what we already delivered by a lot: stale, drop. (A little older is still fresh data for
            // other entities: states of one tick are spread over several messages.)
            if (anyDelivered_ && (int16_t)(seq - lastDelivered_) < -32) { ++lost_; return true; }
            size_t off = n - r.remaining();
            Partial& part = partial_[seq];
            if (part.frags.empty()) { part.frags.resize(count); part.got.assign(count, false); part.have = 0; part.started = now; }
            if (part.frags.size() != count) return false;
            if (!part.got[idx]) { part.frags[idx].assign(p + off, p + n); part.got[idx] = true; ++part.have; }
            if (part.have == count)
            {
                Bytes msg;
                for (size_t f = 0; f < count; ++f) msg.insert(msg.end(), part.frags[f].begin(), part.frags[f].end());
                messages.push_back(msg);
                partial_.erase(seq);
                if (!anyDelivered_ || (int16_t)(seq - lastDelivered_) > 0) lastDelivered_ = seq;
                anyDelivered_ = true;
            }
            // forget fragments that never completed
            for (std::map<uint16_t, Partial>::iterator it = partial_.begin(); it != partial_.end();)
                if (now - it->second.started > 2000) { partial_.erase(it++); ++lost_; } else ++it;
        }
        return true;
    }

    unsigned rtt() const { return srtt_; }
    uint32_t resent() const { return resent_; }
    uint32_t lost() const { return lost_; }

private:
    struct Segment { Bytes data; unsigned sentAt, firstSent; int tries, nacks; Segment() : sentAt(0), firstSent(0), tries(0), nacks(0) {} };
    struct Partial { std::vector<Bytes> frags; std::vector<bool> got; size_t have; unsigned started; Partial() : have(0), started(0) {} };

    unsigned rto_() const { unsigned r = srtt_ * 2 + 30; return r < UDP_MIN_RTO ? UDP_MIN_RTO : (r > UDP_MAX_RTO ? UDP_MAX_RTO : r); }

    void header(ByteWriter& w, uint8_t kind)
    {
        w.u8(kind); w.u32(nextExpected_);
        uint32_t bits = 0;
        for (int i = 0; i < 32; ++i) if (received_.count(nextExpected_ + 1 + i)) bits |= 1u << i;
        w.u32(bits);
    }
    Bytes segmentDatagram(uint32_t seq, const Bytes& data)
    {
        ByteWriter w; header(w, DGRAM_RELIABLE); w.u32(seq); w.bytes(data);
        ackPending_ = false;
        return w.data;
    }
    void onAck(uint32_t ack, uint32_t bits, unsigned now)
    {
        // everything before `ack` arrived
        while (!inflight_.empty())
        {
            std::map<uint32_t, Segment>::iterator it = inflight_.begin();
            if ((int32_t)(it->first - ack) >= 0) break;
            if (it->second.tries == 1) sampleRtt(now - it->second.firstSent);
            inflight_.erase(it);
        }
        bool any = false;
        for (int i = 0; i < 32; ++i)
            if (bits & (1u << i))
            {
                std::map<uint32_t, Segment>::iterator it = inflight_.find(ack + 1 + i);
                if (it != inflight_.end()) { if (it->second.tries == 1) sampleRtt(now - it->second.firstSent); inflight_.erase(it); any = true; }
            }
        // later segments arrived but `ack` did not: count it as a hint it was lost (fast resend after 3)
        if (any) { std::map<uint32_t, Segment>::iterator miss = inflight_.find(ack); if (miss != inflight_.end()) ++miss->second.nacks; }
    }
    void sampleRtt(unsigned ms) { if (ms > 5000) return; srtt_ = (srtt_ * 7 + ms) / 8; if (srtt_ < 1) srtt_ = 1; }

    Bytes pendingStream_;
    std::map<uint32_t, Segment> inflight_;
    uint32_t nextSeq_, nextExpected_;
    std::map<uint32_t, Bytes> received_;
    bool ackPending_;
    unsigned srtt_;
    uint16_t useq_, lastDelivered_;
    bool anyDelivered_;
    std::map<uint16_t, Partial> partial_;
    uint32_t resent_, lost_;
};

} // namespace mp
