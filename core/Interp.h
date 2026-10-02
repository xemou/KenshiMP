// Smooth remote movement (engine independent, unit-tested in tests/net_test.cpp).
//
// Like a game server's entity tracking: every state carries the sender's clock; the receiver
//  1. maps sender time to local time (ClockSync: minimum observed one-way offset, so network
//     delay spikes do not shift the timeline),
//  2. plays remote entities back a little in the past (render delay, adapted to the measured
//     jitter) and interpolates between the two snapshots around that time,
//  3. extrapolates with the last known velocity for a short while when packets are late, then
//     holds still (never flies away).
// VS2010 compatible (no <chrono>, no range-for).
#pragma once
#include <stdint.h>
#include <math.h>
#include <deque>

namespace mp {

// Maps a remote millisecond clock (32-bit, wraps) onto the local one.
class ClockSync
{
public:
    ClockSync() : has_(false), offset_(0), windowStart_(0), windowMin_(0), target_(0), jitter_(0), lastArrival_(0), lastSender_(0) {}

    // Call for every received timestamped message.
    void observe(uint32_t senderMs, uint32_t localMs)
    {
        int64_t sample = (int64_t)localMs - (int64_t)senderMs;     // one-way delay + clock offset
        if (!has_) { has_ = true; offset_ = sample; target_ = sample; windowStart_ = localMs; windowMin_ = sample; }
        // Minimum over a sliding ~10 s window: the least-delayed packet reflects the true offset.
        if (sample < windowMin_) windowMin_ = sample;
        if (sample < offset_) { offset_ = sample; target_ = sample; }   // faster path found: adopt now
        if ((uint32_t)(localMs - windowStart_) > 10000)
        {
            target_ = windowMin_;                                   // lets the offset drift upwards too ...
            windowStart_ = localMs;
            windowMin_ = sample;
        }
        // ... but gently (1 ms per message, ~20 ms/s): a sudden step would shift every new snapshot
        // against the buffered ones and make the path zigzag.
        if (target_ > offset_) offset_ += 1;
        // Jitter: deviation of inter-arrival from inter-send (EMA, ms).
        if (lastArrival_)
        {
            double dArr = (double)(int32_t)(localMs - lastArrival_);
            double dSend = (double)(int32_t)(senderMs - lastSender_);
            double dev = fabs(dArr - dSend);
            jitter_ += (dev - jitter_) * 0.1;
        }
        lastArrival_ = localMs; lastSender_ = senderMs;
    }

    bool ready() const { return has_; }
    // Sender time -> local time (ms, same scale as localMs).
    double toLocal(uint32_t senderMs) const { return (double)senderMs + (double)offset_; }
    double jitterMs() const { return jitter_; }

private:
    bool has_;
    int64_t offset_, windowStart_, windowMin_, target_;
    double jitter_;
    uint32_t lastArrival_, lastSender_;
};

struct Snapshot
{
    double t;                    // local time (ms) at which the sender was in this state
    float x, y, z;
    float qx, qy, qz, qw;
    float vx, vy, vz;            // units per second
};

class InterpBuffer
{
public:
    static const int MAX_SNAPSHOTS = 32;
    static double maxExtrapolationMs() { return 400.0; }

    void push(const Snapshot& s)
    {
        // Keep time order; drop duplicates / out-of-order older than the newest.
        if (!buf_.empty() && s.t <= buf_.back().t)
        {
            if (s.t < buf_.front().t) return;
            for (std::deque<Snapshot>::iterator it = buf_.begin(); it != buf_.end(); ++it)
            {
                if (it->t == s.t) return;
                if (it->t > s.t) { buf_.insert(it, s); trim(); return; }
            }
        }
        buf_.push_back(s);
        trim();
    }

    bool empty() const { return buf_.empty(); }
    const Snapshot& latest() const { return buf_.back(); }
    void clear() { buf_.clear(); }

    // State at local time `rt` (already shifted into the past by the render delay).
    // Returns false if there is nothing to sample.
    bool sample(double rt, Snapshot& out) const
    {
        if (buf_.empty()) return false;
        if (rt <= buf_.front().t) { out = buf_.front(); out.t = rt; return true; }
        for (size_t i = 1; i < buf_.size(); ++i)
        {
            const Snapshot& a = buf_[i - 1];
            const Snapshot& b = buf_[i];
            if (rt <= b.t)
            {
                double span = b.t - a.t;
                float k = span > 0 ? (float)((rt - a.t) / span) : 1.0f;
                lerp(a, b, k, out);
                out.t = rt;
                return true;
            }
        }
        // Past the newest snapshot: extrapolate briefly along its velocity, then hold.
        const Snapshot& last = buf_.back();
        double dt = rt - last.t;
        if (dt > maxExtrapolationMs()) dt = maxExtrapolationMs();
        float s = (float)(dt / 1000.0);
        out = last;
        out.x += last.vx * s; out.y += last.vy * s; out.z += last.vz * s;
        out.t = rt;
        return true;
    }

private:
    void trim() { while ((int)buf_.size() > MAX_SNAPSHOTS) buf_.pop_front(); }

    static void lerp(const Snapshot& a, const Snapshot& b, float k, Snapshot& o)
    {
        o.x = a.x + (b.x - a.x) * k; o.y = a.y + (b.y - a.y) * k; o.z = a.z + (b.z - a.z) * k;
        o.vx = a.vx + (b.vx - a.vx) * k; o.vy = a.vy + (b.vy - a.vy) * k; o.vz = a.vz + (b.vz - a.vz) * k;
        // Normalised lerp on the shortest arc (plenty for 50 ms apart).
        float bx = b.qx, by = b.qy, bz = b.qz, bw = b.qw;
        if (a.qx * bx + a.qy * by + a.qz * bz + a.qw * bw < 0) { bx = -bx; by = -by; bz = -bz; bw = -bw; }
        o.qx = a.qx + (bx - a.qx) * k; o.qy = a.qy + (by - a.qy) * k;
        o.qz = a.qz + (bz - a.qz) * k; o.qw = a.qw + (bw - a.qw) * k;
        float n = sqrtf(o.qx * o.qx + o.qy * o.qy + o.qz * o.qz + o.qw * o.qw);
        if (n > 1e-6f) { o.qx /= n; o.qy /= n; o.qz /= n; o.qw /= n; } else { o.qx = o.qy = o.qz = 0; o.qw = 1; }
    }

    std::deque<Snapshot> buf_;
};

// Playback time for one remote sender. It advances with real time and drifts smoothly (at most
// +-MAX_WARP speed change) towards its target (now - render delay), so a change of delay or a
// late burst never makes the displayed motion jump; only a big gap (> SNAP_MS) snaps.
class PlaybackClock
{
public:
    static double maxWarp() { return 0.10; }
    static double snapMs() { return 500.0; }

    PlaybackClock() : has_(false), rt_(0), lastNow_(0) {}

    double update(double now, double target)
    {
        if (!has_ || fabs(target - rt_) > snapMs()) { has_ = true; rt_ = target; lastNow_ = now; return rt_; }
        double dt = now - lastNow_;
        lastNow_ = now;
        if (dt < 0) dt = 0;
        double err = target - (rt_ + dt);
        double maxAdj = dt * maxWarp();
        if (err > maxAdj) err = maxAdj;
        if (err < -maxAdj) err = -maxAdj;
        rt_ += dt + err;
        return rt_;
    }

    void reset() { has_ = false; }

private:
    bool has_;
    double rt_, lastNow_;
};

// Displayed position that follows the sampled one: it moves with the sampled velocity and
// absorbs discontinuities (late burst after a latency spike, extrapolation corrected) by a
// bounded correction speed instead of jumping. Only an error above snapDist teleports.
class SmoothFollower
{
public:
    SmoothFollower() : has_(false), x(0), y(0), z(0) {}

    // dtMs: frame time. maxCorrection: extra speed (units/s) allowed to close the error.
    void update(const Snapshot& s, double dtMs, float maxCorrection = 2.5f, float snapDist = 6.0f)
    {
        float dt = (float)(dtMs / 1000.0);
        if (!has_) { has_ = true; x = s.x; y = s.y; z = s.z; return; }
        float px = x + s.vx * dt, py = y + s.vy * dt, pz = z + s.vz * dt;   // keep moving with it
        float ex = s.x - px, ey = s.y - py, ez = s.z - pz;
        float len = sqrtf(ex * ex + ey * ey + ez * ez);
        if (len > snapDist) { x = s.x; y = s.y; z = s.z; return; }
        float corr = maxCorrection * dt;
        if (len <= corr || len < 1e-5f) { x = s.x; y = s.y; z = s.z; return; }
        float k = corr / len;
        x = px + ex * k; y = py + ey * k; z = pz + ez * k;
    }

    void reset() { has_ = false; }
    bool valid() const { return has_; }
    float x, y, z;

private:
    bool has_;
};

// Render delay: two send intervals plus a jitter margin, bounded.
inline double renderDelayMs(double sendIntervalMs, double jitterMs)
{
    double d = 2.0 * sendIntervalMs + 2.5 * jitterMs;
    if (d < 60.0) d = 60.0;
    if (d > 300.0) d = 300.0;
    return d;
}

} // namespace mp
