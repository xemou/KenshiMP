// Host-side sanity check of the positions players report for their own characters.
// Co-op between friends: this only warns (log + message), it never blocks anything.
//   FAST : average speed above maxSpeed over at least windowMs (fast runners reach ~45 u/s)
//   JUMP : a single step of more than maxJump units in less than a second (teleport)
// Timestamps are the sender's own clock (ms), so network jitter does not create false alarms.
#pragma once
#include <stdint.h>
#include <math.h>
#include <map>

namespace mp {

class MovementGuard
{
public:
    enum Verdict { OK = 0, FAST = 1, JUMP = 2 };

    MovementGuard(float maxSpeed = 80.f, float maxJump = 50.f, uint32_t windowMs = 500)
        : maxSpeed_(maxSpeed), maxJump_(maxJump), windowMs_(windowMs) {}

    // Returns the verdict for this sample; `speed` receives the measured speed (FAST) or jump length (JUMP).
    Verdict observe(uint32_t id, uint32_t tMs, float x, float z, float* speed = 0)
    {
        std::map<uint32_t, Track>::iterator it = tracks_.find(id);
        if (it == tracks_.end())
        {
            Track t; t.lastT = t.anchorT = tMs; t.lastX = t.anchorX = x; t.lastZ = t.anchorZ = z;
            tracks_[id] = t;
            return OK;
        }
        Track& t = it->second;
        uint32_t dtLast = tMs - t.lastT;
        if ((int32_t)dtLast <= 0) return OK;                       // duplicate / out of order
        if (dtLast > 3000) { reset(t, tMs, x, z); return OK; }       // pause, lag: start over
        float step = dist(x, z, t.lastX, t.lastZ);
        t.lastT = tMs; t.lastX = x; t.lastZ = z;
        if (step > maxJump_ && dtLast < 1000)
        {
            if (speed) *speed = step;
            reset(t, tMs, x, z);
            return JUMP;
        }
        uint32_t dtAnchor = tMs - t.anchorT;
        if (dtAnchor < windowMs_) return OK;
        float v = dist(x, z, t.anchorX, t.anchorZ) * 1000.f / (float)dtAnchor;
        t.anchorT = tMs; t.anchorX = x; t.anchorZ = z;
        if (v > maxSpeed_) { if (speed) *speed = v; return FAST; }
        return OK;
    }

    // Forget a player's characters (join, world reload, leave: legitimate teleports follow).
    void forgetOwner(uint8_t owner)
    {
        for (std::map<uint32_t, Track>::iterator it = tracks_.begin(); it != tracks_.end();)
            if ((uint8_t)(it->first >> 24) == owner) tracks_.erase(it++); else ++it;
    }
    size_t tracked() const { return tracks_.size(); }

private:
    struct Track { uint32_t lastT, anchorT; float lastX, lastZ, anchorX, anchorZ; };
    static float dist(float x1, float z1, float x2, float z2) { float dx = x1 - x2, dz = z1 - z2; return sqrtf(dx * dx + dz * dz); }
    static void reset(Track& t, uint32_t tMs, float x, float z) { t.lastT = t.anchorT = tMs; t.lastX = t.anchorX = x; t.lastZ = t.anchorZ = z; }
    std::map<uint32_t, Track> tracks_;
    float maxSpeed_, maxJump_;
    uint32_t windowMs_;
};

} // namespace mp
