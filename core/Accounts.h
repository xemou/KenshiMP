// Dedicated server accounts (DEDICATED_SERVER_PLAN.md, phase 3).
//
// An account = player name + password. The first login with a new name creates it. The server stores per account a
// random salt and the verifier K = PBKDF2-HMAC-SHA256(password, salt, iterations); the password itself is never stored
// and never sent:
//
//   client -> HELLO (name ...)                      server -> MSG_AUTH_CHALLENGE  u8 fresh, str salt, u32 iterations, str nonce
//   client: K = PBKDF2(password, salt, iterations)
//           fresh (new account) : proof = K          (sent once, to register; the only time the verifier travels)
//           otherwise           : proof = HMAC(K, nonce | name)
//   client -> MSG_AUTH_PROOF str proof              server -> MSG_AUTH_RESULT u8 ok, str reason   (then WELCOME as usual)
//
// A replayed proof is useless (new nonce each time). Five wrong answers for one name or one address = refused for a
// minute. The client refuses a server that offers fewer than MIN_ITERATIONS rounds (a weak check).
#pragma once
#include "Crypto.h"
#include "FileUtil.h"
#include <map>
#include <set>

namespace mp {

const uint32_t AUTH_ITERATIONS = 20000;
const uint32_t MIN_ITERATIONS  = 10000;
const int      AUTH_MAX_FAILS  = 5;
const unsigned AUTH_LOCK_MS    = 60000;

struct Account
{
    std::string name;
    Bytes salt, verifier;
    uint32_t iterations;
    bool admin;
    Account() : iterations(AUTH_ITERATIONS), admin(false) {}
};

// What the client computes.
inline Bytes authClientKey(const std::string& password, const Bytes& salt, uint32_t iterations)
{
    return pbkdf2Sha256(password, salt, iterations);
}
inline Bytes authProof(const Bytes& key, const Bytes& nonce, const std::string& name)
{
    Bytes m = nonce; m.insert(m.end(), name.begin(), name.end());
    return hmacSha256(key, m);
}

class AccountStore
{
public:
    AccountStore() : dirty_(false) {}

    // accounts.txt: one line per account "salt_hex iterations verifier_hex flags name" (name = rest of the line).
    bool load(const std::string& path)
    {
        path_ = path;
        accounts_.clear();
        std::string text;
        if (!readFile(path, text)) return false;
        std::vector<std::string> lines = splitLines(text);
        for (size_t i = 0; i < lines.size(); ++i)
        {
            const std::string& l = lines[i];
            if (l.empty() || l[0] == '#') continue;
            std::vector<std::string> w = splitWords(l);
            if (w.size() < 5) continue;
            Account a;
            a.salt = fromHex(w[0]); a.iterations = (uint32_t)strtoul(w[1].c_str(), NULL, 10);
            a.verifier = fromHex(w[2]); a.admin = w[3].find('A') != std::string::npos;
            size_t at = 0;   // name = everything after the 4th word
            for (int k = 0; k < 4; ++k) { at = l.find(w[k], at) + w[k].size(); }
            while (at < l.size() && (l[at] == ' ' || l[at] == '\t')) ++at;
            a.name = l.substr(at);
            if (a.salt.empty() || a.verifier.size() != 32 || a.iterations < 1 || a.name.empty()) continue;
            accounts_[a.name] = a;
        }
        return true;
    }
    bool save()
    {
        if (path_.empty()) return false;
        std::string out = "# KenshiMP server accounts: salt iterations verifier flags name (verifier = PBKDF2-HMAC-SHA256 of the password; delete a line to reset that account)\n";
        for (std::map<std::string, Account>::const_iterator it = accounts_.begin(); it != accounts_.end(); ++it)
        {
            char it_[16]; sprintf_s(it_, "%u", it->second.iterations);
            out += toHex(it->second.salt) + " " + it_ + " " + toHex(it->second.verifier) + " " + (it->second.admin ? "A" : "-") + " " + it->first + "\n";
        }
        dirty_ = false;
        return writeFileAtomic(path_, out, 3);
    }

    const Account* find(const std::string& name) const
    {
        std::map<std::string, Account>::const_iterator it = accounts_.find(name);
        return it == accounts_.end() ? NULL : &it->second;
    }
    size_t size() const { return accounts_.size(); }
    bool remove(const std::string& name) { bool r = accounts_.erase(name) != 0; if (r) save(); return r; }
    void setAdmin(const std::string& name, bool on) { std::map<std::string, Account>::iterator it = accounts_.find(name); if (it != accounts_.end()) { it->second.admin = on; save(); } }
    bool isAdmin(const std::string& name) const { const Account* a = find(name); return a && a->admin; }

    // ---- login attempts (server, network thread) ----
    struct Challenge
    {
        bool fresh;          // no account yet: the proof registers it
        Bytes salt, nonce;
        uint32_t iterations;
    };
    Challenge challenge(const std::string& name)
    {
        Challenge c;
        const Account* a = find(name);
        c.fresh = a == NULL;
        c.salt = a ? a->salt : randomBytes(16);
        c.iterations = a ? a->iterations : AUTH_ITERATIONS;
        c.nonce = randomBytes(16);
        return c;
    }
    // true = logged in (or registered). `why` explains a refusal. now = GetTickCount(), injected for tests.
    bool verify(const std::string& name, const std::string& address, const Challenge& c, const Bytes& proof, std::string& why, unsigned now)
    {
        if (lockedOut(name, address, now)) { why = "too many wrong passwords: try again in a minute"; return false; }
        const Account* a = find(name);
        if (c.fresh)
        {
            if (a) { why = "this name was registered meanwhile: log in again"; return false; }
            if (proof.size() != 32) { fail(name, address, now); why = "bad registration"; return false; }
            Account n; n.name = name; n.salt = c.salt; n.iterations = c.iterations; n.verifier = proof;
            accounts_[name] = n;
            save();
            return true;
        }
        if (!a) { why = "unknown account"; return false; }
        if (!sameBytes(authProof(a->verifier, c.nonce, name), proof))
        {
            fail(name, address, now);
            why = "wrong password for this player name (the server keeps one password per name; ask the admin to reset it if you forgot it)";
            return false;
        }
        fails_.erase("n:" + name); fails_.erase("a:" + address);
        return true;
    }
    bool lockedOut(const std::string& name, const std::string& address, unsigned now)
    {
        return locked("n:" + name, now) || locked("a:" + address, now);
    }

private:
    struct Fails { int count; unsigned since; Fails() : count(0), since(0) {} };
    bool locked(const std::string& key, unsigned now)
    {
        std::map<std::string, Fails>::iterator it = fails_.find(key);
        if (it == fails_.end() || it->second.count < AUTH_MAX_FAILS) return false;
        if (now - it->second.since >= AUTH_LOCK_MS) { fails_.erase(it); return false; }
        return true;
    }
    void fail(const std::string& name, const std::string& address, unsigned now)
    {
        const std::string keys[2] = { "n:" + name, "a:" + address };
        for (int i = 0; i < 2; ++i)
        {
            Fails& f = fails_[keys[i]];
            if (f.count == 0 || now - f.since > AUTH_LOCK_MS) { f.count = 0; f.since = now; }
            ++f.count;
            if (f.count >= AUTH_MAX_FAILS) f.since = now;   // the minute starts at the last failure
        }
    }

    std::string path_;
    std::map<std::string, Account> accounts_;
    std::map<std::string, Fails> fails_;
    bool dirty_;
};

// Bans: names and addresses, one per line in bans.txt ("name <player>" / "ip <address>").
class BanList
{
public:
    void load(const std::string& path)
    {
        path_ = path; names_.clear(); ips_.clear();
        std::string text; if (!readFile(path, text)) return;
        std::vector<std::string> lines = splitLines(text);
        for (size_t i = 0; i < lines.size(); ++i)
        {
            const std::string& l = lines[i];
            if (l.compare(0, 5, "name ") == 0) names_.insert(l.substr(5));
            else if (l.compare(0, 3, "ip ") == 0) ips_.insert(l.substr(3));
        }
    }
    bool banned(const std::string& name, const std::string& ip) const { return names_.count(name) || ips_.count(ip); }
    void banName(const std::string& n) { names_.insert(n); save(); }
    void banIp(const std::string& ip) { ips_.insert(ip); save(); }
    bool unban(const std::string& what) { bool r = names_.erase(what) + ips_.erase(what) > 0; if (r) save(); return r; }
    std::string list() const
    {
        std::string s;
        for (std::set<std::string>::const_iterator it = names_.begin(); it != names_.end(); ++it) s += (s.empty() ? "" : ", ") + *it;
        for (std::set<std::string>::const_iterator it = ips_.begin(); it != ips_.end(); ++it) s += (s.empty() ? "" : ", ") + *it;
        return s.empty() ? "(none)" : s;
    }
private:
    void save()
    {
        if (path_.empty()) return;
        std::string out;
        for (std::set<std::string>::const_iterator it = names_.begin(); it != names_.end(); ++it) out += "name " + *it + "\n";
        for (std::set<std::string>::const_iterator it = ips_.begin(); it != ips_.end(); ++it) out += "ip " + *it + "\n";
        writeFileAtomic(path_, out, 1);
    }
    std::string path_;
    std::set<std::string> names_, ips_;
};

} // namespace mp
