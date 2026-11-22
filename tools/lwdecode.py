#!/usr/bin/env python3
"""Decode lastwords exports, print them like lw_print(), group crashes.

    lwdecode.py dump.bin                       # same text lw_print() gives
    lwdecode.py uart.log                       # finds hex between the export markers
    lwdecode.py --crashes dev*.bin [--elf fw.elf]

--crashes collapses identical crashes across any number of exports, by
signature. With --elf, crashes are also grouped by the function names at pc
and lr, which stay the same when a rebuild moves the addresses.
"""
import argparse
import os
import re
import struct
import sys

MAGIC = b"LWX1"
T_TEXT, T_BOOT, T_FAULT, T_PANIC, T_BLOB, T_REPEAT = range(6)
STACK_WORDS = 16
BEGIN, END = "---- export ----", "---- end export ----"

CFSR_NAMES = [(0, "IACCVIOL"), (1, "DACCVIOL"), (3, "MUNSTKERR"), (4, "MSTKERR"), (5, "MLSPERR"),
              (8, "IBUSERR"), (9, "PRECISERR"), (10, "IMPRECISERR"), (11, "UNSTKERR"), (12, "STKERR"),
              (13, "LSPERR"), (16, "UNDEFINSTR"), (17, "INVSTATE"), (18, "INVPC"), (19, "NOCP"),
              (20, "STKOF"), (24, "UNALIGNED"), (25, "DIVBYZERO")]
EXC = {3: "HardFault", 4: "MemManage", 5: "BusFault", 6: "UsageFault", 7: "SecureFault"}


class DecodeError(Exception):
    pass


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def fnv_words(words):
    h = 2166136261
    for w in words:
        for i in range(4):
            h = ((h ^ ((w >> (8 * i)) & 0xFF)) * 16777619) & 0xFFFFFFFF
    return h


def le32(b, off=0):
    return struct.unpack_from("<I", b, off)[0] if len(b) >= off + 4 else 0


def signature(rtype, data):
    """Same value as lw_signature() on the device."""
    if rtype == T_REPEAT:
        return le32(data) if len(data) >= 4 else 0
    if rtype == T_FAULT and len(data) >= 52:
        h = fnv_words([rtype, le32(data, 40), le32(data, 44), le32(data, 48) & 0x40000002,
                       le32(data, 20) & ~1 & 0xFFFFFFFF, le32(data, 24) & ~1 & 0xFFFFFFFF])
    elif rtype == T_PANIC and len(data) >= 4:
        h = fnv_words([rtype, le32(data) & ~1 & 0xFFFFFFFF])
    else:
        return 0
    return h or 1


class Reader:
    def __init__(self, buf):
        self.b, self.i = buf, 0

    def byte(self):
        if self.i >= len(self.b):
            raise DecodeError("truncated")
        self.i += 1
        return self.b[self.i - 1]

    def var(self):
        v = shift = 0
        while True:
            c = self.byte()
            v |= (c & 0x7F) << shift
            if not c & 0x80:
                return v
            shift += 7
            if shift > 28:
                raise DecodeError("bad varint")

    def take(self, n):
        if self.i + n > len(self.b):
            raise DecodeError("truncated")
        self.i += n
        return self.b[self.i - n:self.i]


def decode(buf):
    """Export bytes -> (boot, [dict(type, level, seq, time, data)])."""
    if len(buf) < 9 or buf[:4] != MAGIC:
        raise DecodeError("not a lastwords export")
    if crc16(buf[:-2]) != struct.unpack_from("<H", buf, len(buf) - 2)[0]:
        raise DecodeError("CRC mismatch")
    r = Reader(buf[:-2])
    r.take(4)
    boot, count, seq = r.var(), r.var(), r.var()
    t = 0
    out = []
    for _ in range(count):
        tl = r.byte()
        z = r.var()
        t = (t + ((z >> 1) ^ -(z & 1))) & 0xFFFFFFFF
        payload = Reader(r.take(r.var()))
        rtype = tl >> 4
        if rtype == T_FAULT and (nw := payload.byte()):
            bitmap = payload.take((nw + 7) // 8)
            words = [le32(payload.take(4)) if bitmap[i // 8] >> (i % 8) & 1 else 0 for i in range(nw)]
            data = struct.pack(f"<{nw}I", *words)
        else:  # for faults, the 0 "unpacked" marker was read by the test above
            data = payload.take(len(payload.b) - payload.i)
        if payload.i != len(payload.b):
            raise DecodeError("payload length mismatch")
        out.append({"type": rtype, "level": tl & 0xF, "seq": seq, "time": t, "data": bytes(data)})
        seq = (seq + 1) & 0xFFFF
    if r.i != len(r.b):
        raise DecodeError("trailing bytes")
    return boot, out


class Line:
    """Mirror of lw_print's line buffer, wrapping included."""

    def __init__(self, out):
        self.buf = bytearray()
        self.out = out

    def flush(self):
        self.out.append(bytes(self.buf) + b"\n")
        self.buf = bytearray()

    def ch(self, c):
        if len(self.buf) >= 94:
            self.flush()
            self.ch(32)
            self.ch(32)
        self.buf.append(c)

    def s(self, text):
        for c in text.encode():
            self.ch(c)

    def kv(self, k, v):
        self.s(f" {k}=0x{v & 0xFFFFFFFF:08x}")


def print_entry(e, l):
    l.s(f"[{e['time']:8d}] #{e['seq']:5d} ")
    d, rtype = e["data"], e["type"]
    if rtype == T_TEXT:
        l.s("DIWEF"[e["level"]] if e["level"] < 5 else "?")
        l.ch(32)
        for c in d:
            l.ch(c if 0x20 <= c < 0x7F or c >= 0x80 else 0x2E)
        l.flush()
    elif rtype == T_BOOT:
        l.s(f"-- boot {le32(d) if len(d) >= 4 else 0}")
        if len(d) >= 8:
            l.kv("reset_cause", le32(d, 4))
        l.flush()
    elif rtype == T_FAULT:
        w = list(struct.unpack_from(f"<{min(len(d), 128) // 4}I", d)) + [0] * 32
        r0, r1, r2, r3, r12, lr, pc, xpsr, _, sp, ipsr, cfsr, hfsr, mmfar, bfar, nstack = w[:16]
        l.s("!! " + EXC.get(ipsr, "exception"))
        for bit, name in CFSR_NAMES:
            if cfsr >> bit & 1:
                l.s(" " + name)
        if hfsr >> 30 & 1:
            l.s(" FORCED")
        if hfsr >> 1 & 1:
            l.s(" VECTTBL")
        l.flush()
        l.s("  ")
        l.kv("sig", signature(rtype, d))
        l.flush()
        l.s("  ")
        for k, v in (("pc", pc), ("lr", lr), ("sp", sp), ("xpsr", xpsr)):
            l.kv(k, v)
        l.flush()
        l.s("  ")
        for k, v in (("r0", r0), ("r1", r1), ("r2", r2), ("r3", r3), ("r12", r12)):
            l.kv(k, v)
        l.flush()
        if cfsr | hfsr:
            l.s("  ")
            l.kv("cfsr", cfsr)
            l.kv("hfsr", hfsr)
            if cfsr >> 7 & 1:
                l.kv("mmfar", mmfar)
            if cfsr >> 15 & 1:
                l.kv("bfar", bfar)
            l.flush()
        stack = w[16:16 + min(nstack, STACK_WORDS)]
        for i, v in enumerate(stack):
            if i % 4 == 0:
                l.s("   stack:" if i == 0 else "         ")
            l.s(f" 0x{v:08x}")
            if i % 4 == 3 or i + 1 == len(stack):
                l.flush()
    elif rtype == T_PANIC:
        l.s("!! panic")
        if len(d) >= 4:
            l.kv("pc", le32(d))
        l.s(": ")
        for c in d[4:]:
            l.ch(c)
        l.flush()
        l.s("  ")
        l.kv("sig", signature(rtype, d))
        l.flush()
    elif rtype == T_REPEAT:
        l.s("!! repeat")
        l.kv("sig", signature(rtype, d))
        l.s(f" count={d[4] | d[5] << 8 if len(d) >= 6 else 0}")
        l.flush()
    else:
        l.s(f"blob type={rtype} len={len(d)}:")
        for c in d[:16]:
            l.s(f" {c:02x}")
        if len(d) > 16:
            l.s(" ...")
        l.flush()


def render(entries):
    out = []
    l = Line(out)
    for e in entries:
        print_entry(e, l)
    return b"".join(out)


def load(path):
    """Raw export bytes from a .bin, or hex between the markers in a text log."""
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:4] == MAGIC:
        return [raw]
    text = raw.decode("utf-8", "replace")
    blobs = re.findall(re.escape(BEGIN) + r"\s*\n(.*?)" + re.escape(END), text, re.S)
    if not blobs:
        raise DecodeError(f"{path}: no export found")
    return [bytes.fromhex("".join(b.split())) for b in blobs]


def crash_summary(entries):
    """(sig, short description, pc, lr) for every crash record."""
    full = {}
    for e in entries:
        d, rtype = e["data"], e["type"]
        sig = signature(rtype, d)
        if rtype == T_FAULT and sig:
            pc = le32(d, 24)
            # Bad branch target: pc is garbage, only lr says where it came from.
            if not le32(d, 28) >> 24 & 1 or le32(d, 44) & 0x60101:
                pc = 0
            full[sig] = (EXC.get(le32(d, 40), "exception"), pc, le32(d, 20))
            yield sig, full[sig], 1
        elif rtype == T_PANIC and sig:
            full[sig] = ("panic " + d[4:].decode("utf-8", "replace"), le32(d), None)
            yield sig, full[sig], 1
        elif rtype == T_REPEAT and sig:
            yield sig, full.get(sig), 1


def crashes(paths, elf, addr2line, out):
    groups = {}
    res = None
    if elf:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import lwsym
        res = lwsym.Resolver(elf, lwsym.find_addr2line(addr2line))
    for path in paths:
        for blob in load(path):
            _, entries = decode(blob)
            for sig, info, n in crash_summary(entries):
                key = sig
                where = ""
                if info and res:
                    what, pc, lr = info
                    addrs = ([pc] if pc else []) + ([lwsym.call_site(lr)] if lr is not None else [])
                    names = [h[0] if h else "?" for h in res.lookup(addrs)]
                    where = " <- ".join((["(bad jump)"] if not pc else []) + names)
                    key = "sym:" + what.split()[0] + ":" + where
                g = groups.setdefault(key, {"n": 0, "sigs": set(), "files": set(), "info": None, "where": where})
                g["n"] += n
                g["sigs"].add(sig)
                g["files"].add(path)
                g["info"] = g["info"] or info
    out.write(f"{'count':>5}  {'devices':>7}  signature   crash\n")
    for g in sorted(groups.values(), key=lambda g: -g["n"]):
        desc = g["info"][0] if g["info"] else "(full record overwritten)"
        if g["info"] and not g["where"]:
            desc += f" pc=0x{g['info'][1]:08x}" if g["info"][1] else " (bad jump)"
        if g["where"]:
            desc += "  " + g["where"]
        sigs = ",".join(f"{s:08x}" for s in sorted(g["sigs"]))
        out.write(f"{g['n']:5d}  {len(g['files']):7d}  {sigs}  {desc}\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("files", nargs="+", help="export .bin files or logs with hex exports")
    ap.add_argument("--crashes", action="store_true", help="group crashes by signature instead of printing")
    ap.add_argument("--elf", help="firmware ELF: group by function names too (with --crashes)")
    ap.add_argument("--addr2line", help="addr2line binary to use")
    args = ap.parse_args()
    try:
        if args.crashes:
            crashes(args.files, args.elf, args.addr2line, sys.stdout)
            return
        for path in args.files:
            for blob in load(path):
                _, entries = decode(blob)
                sys.stdout.buffer.write(render(entries))
    except DecodeError as e:
        sys.exit(f"lwdecode: {e}")


if __name__ == "__main__":
    main()
