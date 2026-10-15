// Compact entity states (DEDICATED_SERVER_PLAN.md, phase 4: quantisation). Same content as EntityState::write but
// positions as 16-bit offsets from a base point of the message (1/8 unit), orientation as 4 signed bytes, health,
// blood and wounds as 16-bit tenths, velocities as 16-bit hundredths, and the task location / velocity / floor only
// when they carry something. Roughly half the bytes of the full form.
//
// MSG_ENTITY_STATE body: u32 senderClockMs, u16 count; when (count & STATES_COMPACT) the real count is the low 15 bits,
// followed by f32 baseX, baseY, baseZ and that many compact states; otherwise full EntityState records.
#pragma once
#include "Protocol.h"

namespace mp {

const uint16_t STATES_COMPACT = 0x8000;

namespace StateCodec {

struct Base
{
    float x, y, z;
    Base() : x(0), y(0), z(0) {}
    Base(float a, float b, float c) : x(a), y(b), z(c) {}
};

const float POS_SCALE = 8.f;          // 1/8 unit
const float POS_RANGE = 4000.f;       // farther from the base than this: full floats

enum { HAS_TASKPOS = 1, HAS_VELOCITY = 2, HAS_FLOOR = 4, FULL_POS = 8, FULL_TASKPOS = 16, FULL_BLOOD = 32, HAS_TARGETS = 64 };

inline int16_t q16(float v, float scale) { float s = v * scale; s = s > 32767.f ? 32767.f : (s < -32767.f ? -32767.f : s); return (int16_t)(s < 0 ? s - 0.5f : s + 0.5f); }
inline int8_t  q8(float v) { float s = v * 127.f; s = s > 127.f ? 127.f : (s < -127.f ? -127.f : s); return (int8_t)(s < 0 ? s - 0.5f : s + 0.5f); }
inline bool near_(float a, float b) { float d = a - b; return d < POS_RANGE && d > -POS_RANGE; }

inline void writePos(ByteWriter& w, float x, float y, float z, const Base& b, bool full)
{
    if (full) { w.f32(x); w.f32(y); w.f32(z); return; }
    w.u16((uint16_t)q16(x - b.x, POS_SCALE)); w.u16((uint16_t)q16(y - b.y, POS_SCALE)); w.u16((uint16_t)q16(z - b.z, POS_SCALE));
}
inline void readPos(ByteReader& r, float& x, float& y, float& z, const Base& b, bool full)
{
    if (full) { x = r.f32(); y = r.f32(); z = r.f32(); return; }
    x = b.x + (int16_t)r.u16() / POS_SCALE; y = b.y + (int16_t)r.u16() / POS_SCALE; z = b.z + (int16_t)r.u16() / POS_SCALE;
}

inline void write(ByteWriter& w, const EntityState& s, const Base& b)
{
    bool fullPos = !(near_(s.x, b.x) && near_(s.y, b.y) && near_(s.z, b.z));
    bool taskPos = s.tx != s.x || s.ty != s.y || s.tz != s.z;
    bool fullTask = taskPos && !(near_(s.tx, b.x) && near_(s.ty, b.y) && near_(s.tz, b.z));
    bool vel = s.vx != 0 || s.vy != 0 || s.vz != 0;
    bool fullBlood = s.blood > 3000.f || s.blood < 0.f;
    bool targets = s.combatTarget.kind != TargetRef::NONE || s.taskSubject.kind != TargetRef::NONE;
    uint8_t bits = (uint8_t)((taskPos ? HAS_TASKPOS : 0) | (vel ? HAS_VELOCITY : 0) | (s.floor ? HAS_FLOOR : 0) | (fullPos ? FULL_POS : 0) |
                             (fullTask ? FULL_TASKPOS : 0) | (fullBlood ? FULL_BLOOD : 0) | (targets ? HAS_TARGETS : 0));
    w.u32(s.netId); w.u8(bits);
    writePos(w, s.x, s.y, s.z, b, fullPos);
    w.u8((uint8_t)q8(s.qx)); w.u8((uint8_t)q8(s.qy)); w.u8((uint8_t)q8(s.qz)); w.u8((uint8_t)q8(s.qw));
    w.u16((uint16_t)q16(s.health, 10.f));
    w.u8((uint8_t)((s.flags & 31) | ((s.moveSpeed > 7 ? 7 : s.moveSpeed) << 5)));
    if (fullBlood) w.f32(s.blood); else w.u16((uint16_t)q16(s.blood, 10.f));
    w.u8((uint8_t)s.flesh.size()); for (size_t k = 0; k < s.flesh.size(); ++k) w.u16((uint16_t)q16(s.flesh[k], 10.f));
    w.u8((uint8_t)s.bandage.size()); for (size_t k = 0; k < s.bandage.size(); ++k) w.u16((uint16_t)q16(s.bandage[k], 10.f));
    w.u16(s.task);
    if (targets) { s.combatTarget.write(w); s.taskSubject.write(w); }
    if (taskPos) writePos(w, s.tx, s.ty, s.tz, b, fullTask);
    if (vel) { w.u16((uint16_t)q16(s.vx, 100.f)); w.u16((uint16_t)q16(s.vy, 100.f)); w.u16((uint16_t)q16(s.vz, 100.f)); }
    if (s.floor) w.u8((uint8_t)s.floor);
}

inline bool read(ByteReader& r, EntityState& s, const Base& b)
{
    s = EntityState();
    s.netId = r.u32(); uint8_t bits = r.u8();
    readPos(r, s.x, s.y, s.z, b, (bits & FULL_POS) != 0);
    s.qx = (int8_t)r.u8() / 127.f; s.qy = (int8_t)r.u8() / 127.f; s.qz = (int8_t)r.u8() / 127.f; s.qw = (int8_t)r.u8() / 127.f;
    sanitizeQuat(s.qx, s.qy, s.qz, s.qw);
    s.health = (int16_t)r.u16() / 10.f;
    uint8_t fm = r.u8(); s.flags = fm & 31; s.moveSpeed = fm >> 5;
    s.blood = (bits & FULL_BLOOD) ? r.f32() : (int16_t)r.u16() / 10.f;
    uint8_t n = r.u8(); for (int k = 0; k < n && r.ok(); ++k) s.flesh.push_back((int16_t)r.u16() / 10.f);
    n = r.u8(); for (int k = 0; k < n && r.ok(); ++k) s.bandage.push_back((int16_t)r.u16() / 10.f);
    s.task = r.u16();
    if (bits & HAS_TARGETS) { s.combatTarget.read(r); s.taskSubject.read(r); }
    if (bits & HAS_TASKPOS) readPos(r, s.tx, s.ty, s.tz, b, (bits & FULL_TASKPOS) != 0); else { s.tx = s.x; s.ty = s.y; s.tz = s.z; }
    if (bits & HAS_VELOCITY) { s.vx = (int16_t)r.u16() / 100.f; s.vy = (int16_t)r.u16() / 100.f; s.vz = (int16_t)r.u16() / 100.f; }
    if (bits & HAS_FLOOR) s.floor = (int8_t)r.u8();
    return r.ok();
}

// Reads the states of one MSG_ENTITY_STATE body after its u32 clock (either form).
inline void readAll(ByteReader& r, std::vector<EntityState>& out)
{
    uint16_t n = r.u16();
    if (!r.ok()) return;
    if (n & STATES_COMPACT)
    {
        n &= ~STATES_COMPACT;
        Base b; b.x = r.f32(); b.y = r.f32(); b.z = r.f32();
        if (!r.ok() || !validPos(b.x, b.y, b.z)) return;
        for (int i = 0; i < n && r.ok(); ++i) { EntityState s; if (read(r, s, b)) out.push_back(s); }
    }
    else for (int i = 0; i < n && r.ok(); ++i) { EntityState s; s.read(r); if (r.ok()) out.push_back(s); }
}

} // namespace StateCodec
} // namespace mp
