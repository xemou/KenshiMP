// Offline binary diff of two builds of Kenshi_x64.exe: every function (from the .pdata exception
// table) gets a signature = its instruction bytes with branch targets, RIP-relative displacements
// and immediates that look like addresses masked out. Functions whose signature is unique in both
// builds are matched; the rest are matched by their position between matched neighbours.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using Iced.Intel;

public class PeImage
{
    public byte[] B; public ulong Base; public List<uint[]> Sections = new List<uint[]>();
    public List<uint[]> Funcs = new List<uint[]>();   // begin, end (RVA)
    public PeImage(string path)
    {
        B = File.ReadAllBytes(path);
        int pe = BitConverter.ToInt32(B, 0x3C);
        int nsec = BitConverter.ToUInt16(B, pe + 6);
        int opt = BitConverter.ToUInt16(B, pe + 20);
        Base = BitConverter.ToUInt64(B, pe + 24 + 24);
        int sec = pe + 24 + opt;
        for (int i = 0; i < nsec; i++)
        {
            int s = sec + 40 * i;
            Sections.Add(new uint[] { BitConverter.ToUInt32(B, s + 12), BitConverter.ToUInt32(B, s + 8), BitConverter.ToUInt32(B, s + 20), BitConverter.ToUInt32(B, s + 16), BitConverter.ToUInt32(B, s + 36) });
        }
        // exception directory = data directory 3
        uint excRva = BitConverter.ToUInt32(B, pe + 24 + 112 + 3 * 8);
        uint excSize = BitConverter.ToUInt32(B, pe + 24 + 112 + 3 * 8 + 4);
        int o = Off(excRva);
        var seen = new HashSet<uint>();
        for (uint k = 0; k + 12 <= excSize; k += 12)
        {
            uint b = BitConverter.ToUInt32(B, o + (int)k), e = BitConverter.ToUInt32(B, o + (int)k + 4);
            if (b != 0 && e > b && seen.Add(b)) Funcs.Add(new uint[] { b, e });
        }
        Funcs.Sort((x, y) => x[0].CompareTo(y[0]));
    }
    public bool IsCode(uint rva)
    {
        foreach (var s in Sections) if (rva >= s[0] && rva < s[0] + Math.Max(s[1], s[3])) return (s[4] & 0x20000000) != 0;
        return false;
    }
    public int Off(uint rva)
    {
        foreach (var s in Sections) if (rva >= s[0] && rva < s[0] + Math.Max(s[1], s[3])) return (int)(s[2] + (rva - s[0]));
        return -1;
    }
}

public static class FuncDiff
{
    public static string Sig(PeImage img, uint begin, uint end)
    {
        int off = img.Off(begin);
        int len = (int)Math.Min(end - begin, 4096);
        if (off < 0 || len <= 0) return null;
        var buf = new byte[len];
        Array.Copy(img.B, off, buf, 0, len);
        var dec = Iced.Intel.Decoder.Create(64, new ByteArrayCodeReader(buf));
        dec.IP = img.Base + begin;
        var sb = new StringBuilder();
        var masked = (byte[])buf.Clone();
        int pos = 0;
        while (pos < len)
        {
            var ins = dec.Decode();
            if (ins.IsInvalid) break;
            int L = ins.Length;
            // x64 layout: ... [displacement][immediate] at the end of the instruction
            int immSize = 0;
            for (int k = 0; k < ins.OpCount; k++)
            {
                switch (ins.GetOpKind(k))
                {
                    case OpKind.Immediate8: case OpKind.Immediate8_2nd: case OpKind.Immediate8to16:
                    case OpKind.Immediate8to32: case OpKind.Immediate8to64: immSize += 1; break;
                    case OpKind.Immediate16: immSize += 2; break;
                    case OpKind.Immediate32: case OpKind.Immediate32to64: immSize += 4; break;
                    case OpKind.Immediate64: immSize += 8; break;
                }
            }
            bool branch = ins.IsCallNear || ins.IsJmpNear || ins.IsJccShortOrNear || ins.IsJmpShort;
            if (branch)
            {
                int n = (ins.IsJmpShort || ins.IsJccShort) ? 1 : 4;
                for (int i = L - n; i < L; i++) masked[pos + i] = 0;
            }
            else
            {
                int ds = ins.IsIPRelativeMemoryOperand ? 4 : (int)ins.MemoryDisplSize;
                if (ds > 4) ds = 4;
                bool hasMem = false;
                for (int k = 0; k < ins.OpCount; k++) if (ins.GetOpKind(k) == OpKind.Memory) hasMem = true;
                if (hasMem && (ins.IsIPRelativeMemoryOperand || ds >= 4))
                    for (int i = L - immSize - ds; i < L - immSize; i++) if (i >= 0) masked[pos + i] = 0;
                if (immSize >= 4)
                    for (int i = L - immSize; i < L; i++) masked[pos + i] = 0;
            }
            pos += L;
        }
        return Convert.ToBase64String(System.Security.Cryptography.SHA1.Create().ComputeHash(masked)) + ":" + len;
    }

    // Masked signature of the first 'bytes' bytes at an address (whole instructions).
    public static string Head(PeImage img, uint rva, int bytes)
    {
        int off = img.Off(rva);
        if (off < 0) return null;
        return Sig(img, rva, rva + (uint)bytes);
    }

    // RIP-relative targets referenced by matched functions: old target -> new target (data and code).
    public static Dictionary<uint, uint> Xrefs(PeImage a, PeImage b, Dictionary<uint, uint> map, Dictionary<uint, uint> funcEnds, Dictionary<uint, uint> funcEndsB)
    {
        // votes: old target -> (new target -> count); only pairs of functions with the same
        // number of references are used (same code shape), then the majority wins.
        var votes = new Dictionary<uint, Dictionary<uint, int>>();
        foreach (var kv in map)
        {
            uint ea; if (!funcEnds.TryGetValue(kv.Key, out ea)) continue;
            uint eb; if (!funcEndsB.TryGetValue(kv.Value, out eb)) continue;
            var la = Targets(a, kv.Key, (int)Math.Min(ea - kv.Key, 16384)); var lb = Targets(b, kv.Value, (int)Math.Min(eb - kv.Value, 16384));
            if (la.Count == 0 || la.Count != lb.Count) continue;
            for (int i = 0; i < la.Count; i++)
            {
                Dictionary<uint, int> v;
                if (!votes.TryGetValue(la[i], out v)) votes[la[i]] = v = new Dictionary<uint, int>();
                int c; v.TryGetValue(lb[i], out c); v[lb[i]] = c + 1;
            }
        }
        var x = new Dictionary<uint, uint>();
        foreach (var kv in votes)
        {
            int total = 0, best = 0; uint bestT = 0;
            foreach (var t in kv.Value) { total += t.Value; if (t.Value > best) { best = t.Value; bestT = t.Key; } }
            if (best * 3 >= total * 2) x[kv.Key] = bestT;
        }
        return x;
    }
    static List<uint> Targets(PeImage img, uint rva, int len)
    {
        var r = new List<uint>();
        int off = img.Off(rva); if (off < 0) return r;
        len = Math.Min(len, img.B.Length - off);
        var buf = new byte[len]; Array.Copy(img.B, off, buf, 0, len);
        var dec = Iced.Intel.Decoder.Create(64, new ByteArrayCodeReader(buf));
        dec.IP = img.Base + rva;
        int pos = 0;
        while (pos < len)
        {
            var ins = dec.Decode();
            if (ins.IsInvalid) break;
            if (ins.IsIPRelativeMemoryOperand) r.Add((uint)(ins.IPRelativeMemoryAddress - img.Base));
            else if (ins.IsCallNear || ins.IsJmpNear) r.Add((uint)(ins.NearBranchTarget - img.Base));
            pos += ins.Length;
            if (ins.Mnemonic == Mnemonic.Int3 && pos > 1) break;
        }
        return r;
    }

    // Returns old RVA -> new RVA for every function of 'oldImg' that could be matched.
    public static Dictionary<uint, uint> Match(PeImage a, PeImage b, out int unique, out int byPosition)
    {
        var sa = new Dictionary<string, List<uint>>();
        var sb = new Dictionary<string, List<uint>>();
        foreach (var f in a.Funcs) { var s = Sig(a, f[0], f[1]); if (s == null) continue; List<uint> l; if (!sa.TryGetValue(s, out l)) sa[s] = l = new List<uint>(); l.Add(f[0]); }
        foreach (var f in b.Funcs) { var s = Sig(b, f[0], f[1]); if (s == null) continue; List<uint> l; if (!sb.TryGetValue(s, out l)) sb[s] = l = new List<uint>(); l.Add(f[0]); }
        var map = new Dictionary<uint, uint>();
        foreach (var kv in sa)
        {
            List<uint> other;
            if (kv.Value.Count == 1 && sb.TryGetValue(kv.Key, out other) && other.Count == 1) map[kv.Value[0]] = other[0];
        }
        unique = map.Count;
        // Position fill: between two matched neighbours with the same number of functions in between, pair them in order.
        var ia = new Dictionary<uint, int>(); for (int i = 0; i < a.Funcs.Count; i++) ia[a.Funcs[i][0]] = i;
        var ib = new Dictionary<uint, int>(); for (int i = 0; i < b.Funcs.Count; i++) ib[b.Funcs[i][0]] = i;
        var anchors = new List<int[]>();
        foreach (var kv in map) anchors.Add(new int[] { ia[kv.Key], ib[kv.Value] });
        anchors.Sort((x, y) => x[0].CompareTo(y[0]));
        byPosition = 0;
        for (int k = 0; k + 1 < anchors.Count; k++)
        {
            int a0 = anchors[k][0], a1 = anchors[k + 1][0], b0 = anchors[k][1], b1 = anchors[k + 1][1];
            if (a1 - a0 != b1 - b0 || a1 - a0 < 2 || b1 < b0) continue;
            for (int d = 1; d < a1 - a0; d++)
            {
                uint fa = a.Funcs[a0 + d][0], fb = b.Funcs[b0 + d][0];
                uint la = a.Funcs[a0 + d][1] - fa, lb = b.Funcs[b0 + d][1] - fb;
                if (!map.ContainsKey(fa) && Math.Abs((int)la - (int)lb) <= Math.Max(16, (int)la / 8)) { map[fa] = fb; byPosition++; }
            }
        }
        return map;
    }
}
