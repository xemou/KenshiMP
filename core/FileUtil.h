// Small file helpers for the dedicated server's data (accounts, player records, logs): atomic writes with rolling
// backups, so a crash in the middle of a save never leaves a truncated file behind.
#pragma once
#include <string>
#include <vector>
#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace mp {

inline bool readFile(const std::string& path, std::string& out)
{
    FILE* f = NULL;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    out.clear();
    char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

// Writes path.tmp, flushes it, then swaps it in. With keep > 0 the previous versions are kept as path.1 (newest) ..
// path.<keep> (oldest): a bad save can be rolled back by hand.
inline bool writeFileAtomic(const std::string& path, const std::string& data, int keep = 0)
{
    std::string tmp = path + ".tmp";
    FILE* f = NULL;
    if (fopen_s(&f, tmp.c_str(), "wb") != 0 || !f) return false;
    bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fflush(f) == 0 && ok;
    fclose(f);
    if (!ok) { DeleteFileA(tmp.c_str()); return false; }
    if (keep > 0 && GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        char a[16], b[16];
        for (int i = keep; i > 1; --i)
        {
            sprintf_s(a, "%d", i - 1); sprintf_s(b, "%d", i);
            MoveFileExA((path + "." + a).c_str(), (path + "." + b).c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        CopyFileA(path.c_str(), (path + ".1").c_str(), FALSE);
    }
    return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

inline void makeDirs(const std::string& dir)
{
    for (size_t i = 0; i <= dir.size(); ++i)
        if (i == dir.size() || dir[i] == '\\' || dir[i] == '/')
            if (i > 2) CreateDirectoryA(dir.substr(0, i).c_str(), NULL);
}

inline std::vector<std::string> splitLines(const std::string& s)
{
    std::vector<std::string> out;
    size_t a = 0;
    while (a < s.size())
    {
        size_t b = s.find('\n', a); if (b == std::string::npos) b = s.size();
        std::string line = s.substr(a, b - a);
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        out.push_back(line);
        a = b + 1;
    }
    return out;
}

inline std::vector<std::string> splitWords(const std::string& s)
{
    std::vector<std::string> out;
    size_t a = 0;
    while (a < s.size())
    {
        while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
        size_t b = a;
        while (b < s.size() && s[b] != ' ' && s[b] != '\t') ++b;
        if (b > a) out.push_back(s.substr(a, b - a));
        a = b;
    }
    return out;
}

// Plain file name made of a player name (accounts, records): letters, digits, '-', '_', everything else as %XX.
inline std::string safeFileName(const std::string& name)
{
    std::string out;
    for (size_t i = 0; i < name.size() && i < 64; ++i)
    {
        unsigned char c = (unsigned char)name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') out += (char)c;
        else { char h[4]; sprintf_s(h, "%%%02X", c); out += h; }
    }
    return out.empty() ? "_" : out;
}

} // namespace mp
