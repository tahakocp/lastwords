#!/usr/bin/env python3
"""Annotate a lastwords dump with function names and source lines.

    lwsym.py firmware.elf < uart.log

Reads the text that lw_print() produced (mixed with any other output),
passes pc, lr and the panic address (RISC-V: mepc and ra) to addr2line,
and lists the stack words that look like return addresses into the
firmware: a cheap backtrace that needs no unwind tables on the device.
Cortex-M and RV32 ELFs are told apart by e_machine.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys

HEX = r"0x[0-9a-fA-F]{8}"


EM_RISCV = 0xF3


def find_addr2line(explicit, riscv=False):
    if explicit:
        return explicit
    cross = ("riscv-none-elf-addr2line", "riscv64-unknown-elf-addr2line") if riscv else ("arm-none-eabi-addr2line",)
    for name in cross + ("llvm-addr2line", "addr2line"):
        path = shutil.which(name)
        if path:
            return path
    sys.exit("lwsym: no addr2line found; pass --addr2line")


def elf_machine(elf):
    with open(elf, "rb") as f:
        head = f.read(20)
    return struct.unpack_from("<H", head, 18)[0] if len(head) == 20 else 0


def code_sections(elf):
    """(addr, bytes) for every executable section of a 32-bit LE ELF."""
    with open(elf, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        sys.exit("lwsym: expected a 32-bit little-endian ELF")
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    out = []
    for i in range(shnum):
        _, typ, flags, addr, off, size = struct.unpack_from("<IIIIII", data, shoff + i * shentsize)
        if typ == 1 and flags & 0x4:  # PROGBITS, SHF_EXECINSTR
            out.append((addr, data[off:off + size]))
    return out


def follows_call_rv(sections, ret):
    """RISC-V: true if ret sits right after a jal/jalr that wrote ra
    (or a compressed c.jal/c.jalr)."""
    if ret & 1:
        return False
    for addr, code in sections:
        if addr + 2 <= ret <= addr + len(code):
            o = ret - addr
            if o >= 4:
                insn, = struct.unpack_from("<I", code, o - 4)
                if insn & 0x7F in (0x6F, 0x67) and insn >> 7 & 0x1F == 1:
                    return True  # jal ra / jalr ra
            hw, = struct.unpack_from("<H", code, o - 2)
            if hw & 0xF07F == 0x9002 and hw >> 7 & 0x1F:
                return True  # c.jalr
            return hw & 0xE003 == 0x2001  # c.jal (RV32 only)
    return False


def follows_call(sections, ret):
    """True if the instruction before ret is BL or BLX: a real return address."""
    ret &= ~1
    for addr, code in sections:
        if addr + 4 <= ret <= addr + len(code):
            o = ret - addr
            hw1, hw2 = struct.unpack_from("<HH", code, o - 4)
            if hw1 & 0xF800 == 0xF000 and hw2 & 0xD000 == 0xD000:
                return True  # BL imm, T1 encoding
            blx, = struct.unpack_from("<H", code, o - 2)
            return blx & 0xFF87 == 0x4780  # BLX register
    return False


class Resolver:
    def __init__(self, elf, addr2line):
        self.elf = elf
        self.tool = addr2line
        self.cache = {}
        self.code = code_sections(elf)
        self.riscv = elf_machine(elf) == EM_RISCV

    def is_return(self, w):
        if self.riscv:
            return follows_call_rv(self.code, w)
        return bool(w & 1) and follows_call(self.code, w)

    def lookup(self, addrs):
        todo = [a for a in addrs if a not in self.cache]
        if todo:
            out = subprocess.run(
                [self.tool, "-f", "-C", "-e", self.elf] + [hex(a) for a in todo],
                check=True, capture_output=True, text=True).stdout.splitlines()
            for i, a in enumerate(todo):
                func, loc = out[2 * i], out[2 * i + 1]
                loc = re.sub(r" \(discriminator \d+\)", "", loc)
                if func == "??":
                    self.cache[a] = None
                else:
                    self.cache[a] = (func, os.path.relpath(loc) if loc.startswith("/") else loc)
        return [self.cache[a] for a in addrs]


def call_site(ret):
    # A return address points after the BL; step back into the call itself
    # so the reported line is the caller's line, not the next one. On
    # RISC-V, ret - 2 lands inside the jal/jalr (or on the c.jal) as well.
    return (ret & ~1) - 2


def annotate(lines, res, out):
    stack = []
    in_fault = False
    bad_jump = None
    first = {}  # crash signature -> "#seq: where" of the full record
    cur = {}

    def remember(**kw):
        cur.update(kw)
        if cur.get("sig") and cur.get("where"):
            first.setdefault(cur["sig"], f"#{cur['seq']}: {cur['where']}")

    def flush_stack():
        if not stack:
            return
        # Thumb return addresses are odd and sit right after a BL/BLX;
        # RISC-V ones are even and sit after a jal/jalr into ra.
        # Stale words from earlier calls still pass, hence "guess".
        cands = [w for w in stack if res.is_return(w)]
        hits = res.lookup([call_site(w) for w in cands]) if cands else []
        frames = [(w, h) for w, h in zip(cands, hits) if h]
        if frames:
            out.write("   backtrace guess (return addresses found on the stack):\n")
            for w, (func, loc) in frames:
                out.write(f"     {w:#010x}  {func}  {loc}\n")
        stack.clear()

    for line in lines:
        stripped = line.rstrip("\n")
        if in_fault and stripped.startswith("   stack:") or (in_fault and stack and re.match(r"^\s+" + HEX, stripped)):
            out.write(line)
            stack.extend(int(w, 16) for w in re.findall(HEX, stripped))
            continue
        flush_stack()
        out.write(line)

        m = re.match(r"^\[\s*\d+\] #\s*(\d+) !! ", stripped)
        if m:
            cur.clear()
            cur["seq"] = m.group(1)
        m = re.match(r"^\s+sig=(" + HEX + ")$", stripped)
        if m:
            remember(sig=m.group(1))
        m = re.search(r"!! repeat sig=(" + HEX + ")", stripped)
        if m:
            cur.clear()
            out.write(f"   same crash as {first.get(m.group(1), 'one no longer in the ring')}\n")
            continue

        if "!! " in stripped and "panic" not in stripped:
            in_fault = True
            # On these the core stacked the bad target as pc; lr is the way back.
            m = re.search(r"\b(INVSTATE|INVPC|IACCVIOL|IBUSERR)\b", stripped)
            bad_jump = m.group(1) if m else None
            m = re.search(r"trap (instruction (?:misaligned|access fault|page fault))", stripped)
            if m:
                bad_jump = m.group(1)  # mepc is the target, ra the way back
        elif re.match(r"^\[\s*\d+\] #", stripped):
            in_fault = False

        m = re.search(r"\bpc=(" + HEX + r") lr=(" + HEX + r") sp=" + HEX + r" xpsr=(" + HEX + ")", stripped)
        if m:
            pc, lr, xpsr = (int(g, 16) for g in m.groups())
            if not xpsr & (1 << 24) and not bad_jump:
                # T bit clear: a branch to an even address. This is the only
                # hint ARMv6-M gives, it has no CFSR.
                bad_jump = "T bit clear"
            names = res.lookup([pc, call_site(lr)])
            for label, hit in (("pc", names[0]), ("lr", names[1])):
                if label == "pc" and bad_jump:
                    out.write(f"   pc -> bad jump target ({bad_jump}), the caller is at lr\n")
                elif hit:
                    out.write(f"   {label} -> {hit[0]}  {hit[1]}\n")
                elif label == "pc":
                    out.write("   pc -> not in this ELF (jump to a bad address?)\n")
                if hit and not (label == "pc" and bad_jump) and "where" not in cur:
                    remember(where=f"{hit[0]}  {hit[1]}")
            continue
        m = re.search(r"\bmepc=(" + HEX + r") ra=(" + HEX + r") sp=" + HEX, stripped)
        if m:
            pc, ra = (int(g, 16) for g in m.groups())
            names = res.lookup([pc, call_site(ra)])
            for label, hit in (("mepc", names[0]), ("ra", names[1])):
                if label == "mepc" and bad_jump:
                    out.write(f"   mepc -> bad jump target ({bad_jump}), the caller is at ra\n")
                elif hit:
                    out.write(f"   {label} -> {hit[0]}  {hit[1]}\n")
                elif label == "mepc":
                    out.write("   mepc -> not in this ELF (jump to a bad address?)\n")
                if hit and not (label == "mepc" and bad_jump) and "where" not in cur:
                    remember(where=f"{hit[0]}  {hit[1]}")
            continue
        m = re.search(r"!! panic pc=(" + HEX + ")", stripped)
        if m:
            hit = res.lookup([call_site(int(m.group(1), 16))])[0]
            if hit:
                out.write(f"   called from {hit[0]}  {hit[1]}\n")
                remember(where=f"{hit[0]}  {hit[1]}")
    flush_stack()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("elf", help="the exact ELF that produced the log")
    ap.add_argument("log", nargs="?", help="log file (default: stdin)")
    ap.add_argument("--addr2line", help="addr2line binary to use")
    args = ap.parse_args()

    res = Resolver(args.elf, find_addr2line(args.addr2line, elf_machine(args.elf) == EM_RISCV))
    src = open(args.log, encoding="utf-8", errors="replace") if args.log else sys.stdin
    with src:
        annotate(src, res, sys.stdout)


if __name__ == "__main__":
    main()
