// Binary (de)serialisation helpers. Written for the VS2010 (v100) toolset: no range-for, no <thread>.
#pragma once
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

namespace mp {

typedef std::vector<uint8_t> Bytes;

class ByteWriter
{
public:
    Bytes data;

    void u8(uint8_t v)   { data.push_back(v); }
    void u16(uint16_t v) { raw(&v, 2); }
    void u32(uint32_t v) { raw(&v, 4); }
    void f32(float v)    { raw(&v, 4); }
    void f64(double v)   { raw(&v, 8); }
    void str(const std::string& s)
    {
        uint16_t n = (uint16_t)(s.size() > 0xFFFF ? 0xFFFF : s.size());
        u16(n);
        if (n) raw(s.data(), n);
    }
    void bytes(const Bytes& b) { if (!b.empty()) raw(&b[0], b.size()); }
    void raw(const void* p, size_t n)
    {
        const uint8_t* b = (const uint8_t*)p;
        data.insert(data.end(), b, b + n);
    }
};

class ByteReader
{
public:
    ByteReader(const uint8_t* p, size_t n) : p_(p), n_(n), pos_(0), ok_(true) {}
    explicit ByteReader(const Bytes& b) : p_(b.empty() ? NULL : &b[0]), n_(b.size()), pos_(0), ok_(true) {}

    uint8_t  u8()  { uint8_t v = 0;  take(&v, 1); return v; }
    uint16_t u16() { uint16_t v = 0; take(&v, 2); return v; }
    uint32_t u32() { uint32_t v = 0; take(&v, 4); return v; }
    float    f32() { float v = 0;    take(&v, 4); return v; }
    double   f64() { double v = 0;   take(&v, 8); return v; }
    std::string str()
    {
        uint16_t n = u16();
        if (!ok_ || pos_ + n > n_) { ok_ = false; return std::string(); }
        std::string s((const char*)p_ + pos_, n);
        pos_ += n;
        return s;
    }
    size_t remaining() const { return n_ - pos_; }
    bool ok() const { return ok_; }

private:
    void take(void* out, size_t n)
    {
        if (!ok_ || pos_ + n > n_) { ok_ = false; return; }
        memcpy(out, p_ + pos_, n);
        pos_ += n;
    }
    const uint8_t* p_;
    size_t n_, pos_;
    bool ok_;
};

} // namespace mp
