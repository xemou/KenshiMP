// SHA-256, HMAC-SHA256 and PBKDF2-HMAC-SHA256 (FIPS 180-4, RFC 2104, RFC 8018), header-only, VC2010-friendly.
// Used by the dedicated server's accounts (core/Accounts.h): passwords are never stored nor sent in clear.
#pragma once
#include "Buffer.h"
#include <string>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")

namespace mp {

class Sha256
{
public:
    Sha256() { reset(); }
    void reset()
    {
        static const uint32_t init[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
        memcpy(h_, init, sizeof(h_)); len_ = 0; fill_ = 0;
    }
    void update(const void* data, size_t n)
    {
        const uint8_t* p = (const uint8_t*)data;
        len_ += n;
        while (n)
        {
            size_t take = 64 - fill_; if (take > n) take = n;
            memcpy(buf_ + fill_, p, take); fill_ += take; p += take; n -= take;
            if (fill_ == 64) { block(buf_); fill_ = 0; }
        }
    }
    void final(uint8_t out[32])
    {
        uint64_t bits = len_ * 8;
        uint8_t pad = 0x80; update(&pad, 1);
        uint8_t zero = 0; while (fill_ != 56) update(&zero, 1);
        uint8_t l[8]; for (int i = 0; i < 8; ++i) l[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(l, 8);
        for (int i = 0; i < 8; ++i) { out[4 * i] = (uint8_t)(h_[i] >> 24); out[4 * i + 1] = (uint8_t)(h_[i] >> 16); out[4 * i + 2] = (uint8_t)(h_[i] >> 8); out[4 * i + 3] = (uint8_t)h_[i]; }
        reset();
    }
private:
    static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* b)
    {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = ((uint32_t)b[4 * i] << 24) | ((uint32_t)b[4 * i + 1] << 16) | ((uint32_t)b[4 * i + 2] << 8) | b[4 * i + 3];
        for (int i = 16; i < 64; ++i)
        {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], bb = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
        for (int i = 0; i < 64; ++i)
        {
            uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += bb; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
    }
    uint32_t h_[8];
    uint64_t len_;
    uint8_t buf_[64];
    size_t fill_;
};

inline Bytes sha256(const Bytes& b)
{
    Sha256 s; if (!b.empty()) s.update(&b[0], b.size());
    Bytes out(32); s.final(&out[0]); return out;
}

inline Bytes hmacSha256(const Bytes& key, const uint8_t* msg, size_t n)
{
    uint8_t k[64]; memset(k, 0, 64);
    if (key.size() > 64) { Bytes h = sha256(key); memcpy(k, &h[0], 32); }
    else if (!key.empty()) memcpy(k, &key[0], key.size());
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    uint8_t inner[32];
    Sha256 s; s.update(ipad, 64); if (n) s.update(msg, n); s.final(inner);
    Bytes out(32);
    s.update(opad, 64); s.update(inner, 32); s.final(&out[0]);
    return out;
}
inline Bytes hmacSha256(const Bytes& key, const Bytes& msg) { return hmacSha256(key, msg.empty() ? NULL : &msg[0], msg.size()); }

// PBKDF2-HMAC-SHA256, one 32-byte block (dkLen = 32).
inline Bytes pbkdf2Sha256(const std::string& password, const Bytes& salt, uint32_t iterations)
{
    Bytes key(password.begin(), password.end());
    Bytes m = salt; m.push_back(0); m.push_back(0); m.push_back(0); m.push_back(1);
    Bytes u = hmacSha256(key, m), t = u;
    for (uint32_t i = 1; i < iterations; ++i)
    {
        u = hmacSha256(key, u);
        for (int j = 0; j < 32; ++j) t[j] ^= u[j];
    }
    return t;
}

// Constant-time comparison.
inline bool sameBytes(const Bytes& a, const Bytes& b)
{
    if (a.size() != b.size()) return false;
    uint8_t d = 0;
    for (size_t i = 0; i < a.size(); ++i) d |= a[i] ^ b[i];
    return d == 0;
}

inline Bytes randomBytes(size_t n)
{
    Bytes out(n);
    HCRYPTPROV prov = 0;
    if (n && CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
    {
        CryptGenRandom(prov, (DWORD)n, &out[0]);
        CryptReleaseContext(prov, 0);
    }
    else for (size_t i = 0; i < n; ++i) out[i] = (uint8_t)(GetTickCount() * 2654435761u >> (i % 24));   // never used in practice
    return out;
}

inline std::string toHex(const Bytes& b)
{
    static const char* d = "0123456789abcdef";
    std::string s; s.reserve(b.size() * 2);
    for (size_t i = 0; i < b.size(); ++i) { s += d[b[i] >> 4]; s += d[b[i] & 15]; }
    return s;
}
inline Bytes fromHex(const std::string& s)
{
    Bytes out;
    for (size_t i = 0; i + 1 < s.size(); i += 2)
    {
        int v = 0;
        for (int k = 0; k < 2; ++k)
        {
            char c = s[i + k]; v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10; else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return Bytes();
        }
        out.push_back((uint8_t)v);
    }
    return out;
}

} // namespace mp
