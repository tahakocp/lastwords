// lastwords export decoder and ELF symbolizer, no dependencies.
// Same output as tools/lwdecode.py (render) and tools/lwsym.py (annotate);
// tests/check_web.mjs compares them on the QEMU demo ELFs.

const MAGIC = [0x4c, 0x57, 0x58, 0x31]; // "LWX1"
const T_TEXT = 0, T_BOOT = 1, T_FAULT = 2, T_PANIC = 3, T_REPEAT = 5, T_TRAP = 6;
const STACK_WORDS = 16;
export const BEGIN = "---- export ----", END = "---- end export ----";

const CFSR_NAMES = [[0, "IACCVIOL"], [1, "DACCVIOL"], [3, "MUNSTKERR"], [4, "MSTKERR"], [5, "MLSPERR"],
  [8, "IBUSERR"], [9, "PRECISERR"], [10, "IMPRECISERR"], [11, "UNSTKERR"], [12, "STKERR"],
  [13, "LSPERR"], [16, "UNDEFINSTR"], [17, "INVSTATE"], [18, "INVPC"], [19, "NOCP"],
  [20, "STKOF"], [24, "UNALIGNED"], [25, "DIVBYZERO"]];
const SFSR_NAMES = ["INVEP", "INVIS", "INVER", "AUVIOL", "INVTRAN", "LSPERR", null, "LSERR"];
const EXC = { 3: "HardFault", 4: "MemManage", 5: "BusFault", 6: "UsageFault", 7: "SecureFault" };
const TRAP_NAMES = ["instruction misaligned", "instruction access fault", "illegal instruction", "breakpoint",
  "load misaligned", "load access fault", "store misaligned", "store access fault",
  "ecall from U", "ecall from S", null, "ecall from M",
  "instruction page fault", "load page fault", null, "store page fault"];

export class DecodeError extends Error {}

function crc16(b, n) {
  let crc = 0xffff;
  for (let i = 0; i < n; i++) {
    crc ^= b[i] << 8;
    for (let k = 0; k < 8; k++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  }
  return crc;
}

function fnvWords(words) {
  let h = 2166136261;
  for (const w of words)
    for (let i = 0; i < 4; i++) h = Math.imul(h ^ ((w >>> (8 * i)) & 0xff), 16777619) >>> 0;
  return h;
}

function le32(b, off = 0) {
  return b.length >= off + 4 ? (b[off] | b[off + 1] << 8 | b[off + 2] << 16 | b[off + 3] << 24) >>> 0 : 0;
}

export function signature(rtype, d) {
  let h;
  if (rtype === T_REPEAT) return d.length >= 4 ? le32(d) : 0;
  if (rtype === T_FAULT && d.length >= 52)
    h = fnvWords([rtype, le32(d, 40), le32(d, 44), (le32(d, 48) & 0x40000002) >>> 0,
      (le32(d, 20) & ~1) >>> 0, (le32(d, 24) & ~1) >>> 0]);
  else if (rtype === T_TRAP && d.length >= 20)
    h = fnvWords([rtype, le32(d, 0), (le32(d, 16) & ~1) >>> 0, (le32(d, 4) & ~1) >>> 0]);
  else if (rtype === T_PANIC && d.length >= 4)
    h = fnvWords([rtype, (le32(d) & ~1) >>> 0]);
  else return 0;
  return h || 1;
}

class Reader {
  constructor(b) { this.b = b; this.i = 0; }
  byte() {
    if (this.i >= this.b.length) throw new DecodeError("truncated");
    return this.b[this.i++];
  }
  var() {
    let v = 0, shift = 0;
    for (;;) {
      const c = this.byte();
      v += (c & 0x7f) * 2 ** shift;
      if (!(c & 0x80)) return v;
      shift += 7;
      if (shift > 28) throw new DecodeError("bad varint");
    }
  }
  take(n) {
    if (this.i + n > this.b.length) throw new DecodeError("truncated");
    this.i += n;
    return this.b.subarray(this.i - n, this.i);
  }
}

// Export bytes -> {boot, entries: [{type, level, seq, time, data}]}
export function decode(buf) {
  if (buf.length < 9 || MAGIC.some((m, i) => buf[i] !== m)) throw new DecodeError("not a lastwords export");
  if (crc16(buf, buf.length - 2) !== (buf[buf.length - 2] | buf[buf.length - 1] << 8))
    throw new DecodeError("CRC mismatch");
  const r = new Reader(buf.subarray(0, buf.length - 2));
  r.take(4);
  const boot = r.var(), count = r.var();
  let seq = r.var(), t = 0;
  const entries = [];
  for (let n = 0; n < count; n++) {
    const tl = r.byte();
    const z = r.var();
    t = (t + ((z - (z % 2)) / 2 ^ -(z & 1))) >>> 0;
    const p = new Reader(r.take(r.var()));
    const rtype = tl >> 4;
    let data, nw;
    if ((rtype === T_FAULT || rtype === T_TRAP) && (nw = p.byte())) {
      const bitmap = p.take((nw + 7) >> 3);
      data = new Uint8Array(nw * 4);
      for (let i = 0; i < nw; i++)
        if (bitmap[i >> 3] >> (i & 7) & 1) data.set(p.take(4), i * 4);
    } else {
      data = p.take(p.b.length - p.i).slice();
    }
    if (p.i !== p.b.length) throw new DecodeError("payload length mismatch");
    entries.push({ type: rtype, level: tl & 15, seq, time: t, data });
    seq = (seq + 1) & 0xffff;
  }
  if (r.i !== r.b.length) throw new DecodeError("trailing bytes");
  return { boot, entries };
}

// Raw exports from a .bin or from the hex between the markers in a log.
export function load(bytes) {
  if (MAGIC.every((m, i) => bytes[i] === m)) return [bytes];
  const text = new TextDecoder().decode(bytes);
  const out = [];
  const re = new RegExp(esc(BEGIN) + "\\s*\\n([\\s\\S]*?)" + esc(END), "g");
  for (const m of text.matchAll(re)) {
    const hex = m[1].replace(/\s+/g, "");
    if (!/^([0-9a-fA-F]{2})*$/.test(hex)) throw new DecodeError("bad hex in export");
    out.push(Uint8Array.from(hex.match(/../g) || [], (h) => parseInt(h, 16)));
  }
  if (!out.length) throw new DecodeError("no export found");
  return out;
}

function esc(s) { return s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"); }
const hex8 = (v) => "0x" + (v >>> 0).toString(16).padStart(8, "0");

// Mirror of lw_print's 94-column line buffer. Strings hold one byte per char.
class Line {
  constructor() { this.buf = ""; this.out = []; }
  flush() { this.out.push(this.buf + "\n"); this.buf = ""; }
  ch(c) {
    if (this.buf.length >= 94) { this.flush(); this.ch(32); this.ch(32); }
    this.buf += String.fromCharCode(c);
  }
  s(text) { for (let i = 0; i < text.length; i++) this.ch(text.charCodeAt(i)); }
  kv(k, v) { this.s(` ${k}=${hex8(v)}`); }
}

function trapName(mcause) {
  if (mcause >>> 31) return `interrupt ${mcause & 0x7fffffff}`;
  if (mcause < TRAP_NAMES.length && TRAP_NAMES[mcause]) return TRAP_NAMES[mcause];
  return `cause ${mcause}`;
}

function words(d, max, pad) {
  const n = Math.min(d.length, max) >> 2, w = [];
  for (let i = 0; i < n; i++) w.push(le32(d, 4 * i));
  for (let i = 0; i < pad; i++) w.push(0);
  return w;
}

function printStack(l, stack) {
  stack.forEach((v, i) => {
    if (i % 4 === 0) l.s(i === 0 ? "   stack:" : "         ");
    l.s(" " + hex8(v));
    if (i % 4 === 3 || i + 1 === stack.length) l.flush();
  });
}

function printEntry(e, l) {
  l.s(`[${String(e.time).padStart(8)}] #${String(e.seq).padStart(5)} `);
  const d = e.data, rtype = e.type;
  if (rtype === T_TEXT) {
    l.s(e.level < 5 ? "DIWEF"[e.level] : "?");
    l.ch(32);
    for (const c of d) l.ch((c >= 0x20 && c < 0x7f) || c >= 0x80 ? c : 0x2e);
    l.flush();
  } else if (rtype === T_BOOT) {
    l.s(`-- boot ${d.length >= 4 ? le32(d) : 0}`);
    if (d.length >= 8) l.kv("reset_cause", le32(d, 4));
    l.flush();
  } else if (rtype === T_FAULT) {
    const w = words(d, 128, 32);
    const [r0, r1, r2, r3, r12, lr, pc, xpsr, excReturn, sp, ipsr, cfsr, hfsr, mmfar, bfar] = w;
    const nstack = Math.min(w[15], STACK_WORDS);
    const tail = 64 + 4 * nstack;
    const [sfsr, sfar] = d.length >= tail + 8 ? [le32(d, tail), le32(d, tail + 4)] : [0, 0];
    l.s("!! " + (EXC[ipsr] || "exception"));
    for (const [bit, name] of CFSR_NAMES) if (cfsr >>> bit & 1) l.s(" " + name);
    if (hfsr >>> 30 & 1) l.s(" FORCED");
    if (hfsr >>> 1 & 1) l.s(" VECTTBL");
    SFSR_NAMES.forEach((name, bit) => { if (sfsr >>> bit & 1 && name) l.s(" " + name); });
    if (excReturn >>> 24 === 0xff && (excReturn & 0x41) === 0x01) l.s(" in non-secure code");
    l.flush();
    l.s("  "); l.kv("sig", signature(rtype, d)); l.flush();
    l.s("  ");
    for (const [k, v] of [["pc", pc], ["lr", lr], ["sp", sp], ["xpsr", xpsr]]) l.kv(k, v);
    l.flush();
    l.s("  ");
    for (const [k, v] of [["r0", r0], ["r1", r1], ["r2", r2], ["r3", r3], ["r12", r12]]) l.kv(k, v);
    l.flush();
    if (cfsr | hfsr) {
      l.s("  "); l.kv("cfsr", cfsr); l.kv("hfsr", hfsr);
      if (cfsr >>> 7 & 1) l.kv("mmfar", mmfar);
      if (cfsr >>> 15 & 1) l.kv("bfar", bfar);
      l.flush();
    }
    if (sfsr) {
      l.s("  "); l.kv("sfsr", sfsr);
      if (sfsr >>> 6 & 1) l.kv("sfar", sfar);
      l.flush();
    }
    printStack(l, w.slice(16, 16 + nstack));
  } else if (rtype === T_TRAP) {
    const w = words(d, 108, 27);
    const [mcause, mepc, mtval, mstatus, ra, sp, a0, a1, a2, a3, nstack] = w;
    l.s("!! trap " + trapName(mcause)); l.flush();
    l.s("  "); l.kv("sig", signature(rtype, d)); l.flush();
    for (const regs of [[["mepc", mepc], ["ra", ra], ["sp", sp], ["mtval", mtval]],
      [["mcause", mcause], ["mstatus", mstatus]], [["a0", a0], ["a1", a1], ["a2", a2], ["a3", a3]]]) {
      l.s("  ");
      for (const [k, v] of regs) l.kv(k, v);
      l.flush();
    }
    printStack(l, w.slice(11, 11 + Math.min(nstack, STACK_WORDS)));
  } else if (rtype === T_PANIC) {
    l.s("!! panic");
    if (d.length >= 4) l.kv("pc", le32(d));
    l.s(": ");
    for (const c of d.subarray(4)) l.ch(c);
    l.flush();
    l.s("  "); l.kv("sig", signature(rtype, d)); l.flush();
  } else if (rtype === T_REPEAT) {
    l.s("!! repeat"); l.kv("sig", signature(rtype, d));
    l.s(` count=${d.length >= 6 ? d[4] | d[5] << 8 : 0}`);
    l.flush();
  } else {
    l.s(`blob type=${rtype} len=${d.length}:`);
    for (const c of d.subarray(0, 16)) l.s(" " + c.toString(16).padStart(2, "0"));
    if (d.length > 16) l.s(" ...");
    l.flush();
  }
}

// Same bytes lw_print() writes, as a one-byte-per-char string.
export function render(entries) {
  const l = new Line();
  for (const e of entries) printEntry(e, l);
  return l.out.join("");
}

/* ---------------- ELF + DWARF ---------------- */

const EM_RISCV = 0xf3;

class Buf {
  constructor(u8, off = 0) {
    this.u = u8; this.v = new DataView(u8.buffer, u8.byteOffset, u8.byteLength); this.i = off;
  }
  u8() { return this.u[this.i++]; }
  u16() { const x = this.v.getUint16(this.i, true); this.i += 2; return x; }
  u32() { const x = this.v.getUint32(this.i, true); this.i += 4; return x; }
  u64() { const lo = this.u32(), hi = this.u32(); return hi * 2 ** 32 + lo; }
  i8() { const x = this.v.getInt8(this.i); this.i += 1; return x; }
  uleb() {
    let v = 0, mul = 1, c;
    do { c = this.u[this.i++]; v += (c & 0x7f) * mul; mul *= 128; } while (c & 0x80);
    return v;
  }
  sleb() {
    let v = 0, mul = 1, c;
    do { c = this.u[this.i++]; v += (c & 0x7f) * mul; mul *= 128; } while (c & 0x80);
    if (c & 0x40) v -= mul;
    return v;
  }
  cstr() {
    let e = this.i;
    while (this.u[e]) e++;
    const s = cstrAt(this.u, this.i);
    this.i = e + 1;
    return s;
  }
  addr(size) { return size === 8 ? this.u64() : size === 2 ? this.u16() : this.u32(); }
}

function cstrAt(u8, off) {
  if (!u8 || off >= u8.length) return "";
  let e = off;
  while (e < u8.length && u8[e]) e++;
  return new TextDecoder().decode(u8.subarray(off, e));
}

export function normpath(p) {
  const abs = p.startsWith("/"), out = [];
  for (const part of p.split("/")) {
    if (!part || part === ".") continue;
    if (part === ".." && out.length && out[out.length - 1] !== "..") out.pop();
    else if (part !== ".." || !abs) out.push(part);
  }
  return (abs ? "/" : "") + out.join("/") || (abs ? "/" : ".");
}

// os.path.relpath(path, start) for absolute POSIX paths
export function relpath(path, start) {
  const a = normpath(path).split("/").filter(Boolean), b = normpath(start).split("/").filter(Boolean);
  let i = 0;
  while (i < a.length && i < b.length && a[i] === b[i]) i++;
  const parts = Array(b.length - i).fill("..").concat(a.slice(i));
  return parts.join("/") || ".";
}

function joinpath(dir, name) {
  if (name.startsWith("/") || !dir) return name;
  return dir.endsWith("/") ? dir + name : dir + "/" + name;
}

export class Elf {
  constructor(bytes) {
    const u = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    if (u[0] !== 0x7f || u[1] !== 0x45 || u[2] !== 0x4c || u[3] !== 0x46) throw new Error("not an ELF file");
    if (u[4] !== 1 || u[5] !== 1) throw new Error("expected a 32-bit little-endian ELF");
    this.u = u;
    const b = new Buf(u);
    b.i = 18; this.machine = b.u16();
    b.i = 0x20; const shoff = b.u32();
    b.i = 0x2e; const shentsize = b.u16(), shnum = b.u16(), shstrndx = b.u16();
    const sh = [];
    for (let k = 0; k < shnum; k++) {
      b.i = shoff + k * shentsize;
      const s = { nameOff: b.u32(), type: b.u32(), flags: b.u32(), addr: b.u32(), off: b.u32(), size: b.u32(), link: b.u32() };
      b.i += 4; b.u32(); s.entsize = b.u32();
      sh.push(s);
    }
    const strtab = sh[shstrndx];
    for (const s of sh) {
      s.name = strtab ? cstrAt(u.subarray(strtab.off, strtab.off + strtab.size), s.nameOff) : "";
      s.data = s.type === 8 ? new Uint8Array(0) : u.subarray(s.off, s.off + s.size);
    }
    this.sections = sh;
    this.riscv = this.machine === EM_RISCV;
    this.code = sh.filter((s) => s.type === 1 && s.flags & 4).map((s) => [s.addr, s.data]);
    this.sec = (n) => { const s = sh.find((x) => x.name === n); return s ? s.data : null; };
    this.readSymbols();
    this.units = [];
    try { this.readDwarf(); } catch (e) { this.dwarfError = e.message; }
    this.cache = new Map();
  }

  // Function symbols in symtab order, with the STT_FILE name binutils
  // would attach: locals get the last FILE before them, globals only
  // while no FILE has followed a symbol yet.
  readSymbols() {
    this.syms = [];
    const st = this.sections.find((s) => s.type === 2);
    if (!st) return;
    const strs = this.sections[st.link].data;
    const b = new Buf(st.data);
    let file = null, seen = 0;
    for (let k = 1; k < st.data.length / 16; k++) {
      b.i = k * 16;
      const name = b.u32(), value = b.u32(), size = b.u32(), info = b.u8(); b.u8();
      const shndx = b.u16(), type = info & 15, local = info >> 4 === 0;
      if (type === 4) { file = cstrAt(strs, name); if (seen === 1) seen = 2; continue; }
      if (type !== 2 && type !== 0) continue;
      if (!shndx || shndx >= 0xff00 || !name || strs[name] === 0x24) continue; // $t/$d/$x mapping symbols
      const s = this.sections[shndx];
      if (!s || !(s.flags & 4)) continue;
      this.syms.push({
        name: cstrAt(strs, name), value: this.machine === 40 && type === 2 ? value & ~1 : value,
        size: size || 1, type, shndx, file: file && (local || seen !== 2) ? file : null,
      });
      if (!seen) seen = 1;
    }
  }

  readDwarf() {
    const info = this.sec(".debug_info"), abbrev = this.sec(".debug_abbrev");
    if (!info || !abbrev) return;
    this.str = this.sec(".debug_str"); this.lineStr = this.sec(".debug_line_str");
    this.ranges = this.sec(".debug_ranges"); this.rnglists = this.sec(".debug_rnglists");
    this.addrSec = this.sec(".debug_addr"); this.strOffsets = this.sec(".debug_str_offsets");
    this.line = this.sec(".debug_line");
    const abbrevCache = new Map();
    const b = new Buf(info);
    while (b.i < info.length) {
      const cuOff = b.i;
      let len = b.u32();
      if (len === 0xffffffff) throw new Error("64-bit DWARF is not supported");
      const end = b.i + len;
      const version = b.u16();
      if (version < 2 || version > 5) throw new Error(`DWARF version ${version} is not supported`);
      let unitType = 1, abbrevOff, addrSize;
      if (version >= 5) { unitType = b.u8(); addrSize = b.u8(); abbrevOff = b.u32(); }
      else { abbrevOff = b.u32(); addrSize = b.u8(); }
      if (unitType === 2 || unitType === 6) b.i += 12;
      else if (unitType === 4 || unitType === 5) b.i += 8;
      if (!abbrevCache.has(abbrevOff)) abbrevCache.set(abbrevOff, parseAbbrev(abbrev, abbrevOff));
      const cu = { off: cuOff, version, addrSize, unitType, funcs: [], dies: new Map(), ranges: [] };
      if (unitType === 1 || unitType === 3) this.readUnit(cu, new Buf(info, b.i), end, abbrevCache.get(abbrevOff));
      this.units.push(cu);
      b.i = end;
    }
    for (const cu of this.units) {
      for (const f of cu.funcs) f.name = this.dieName(cu, f.die, 0);
      if (cu.stmtList !== undefined && this.line) cu.lines = this.readLines(cu);
    }
  }

  readUnit(cu, b, end, abbrevs) {
    let order = 0;
    while (b.i < end) {
      const dieOff = b.i;
      const code = b.uleb();
      if (!code) continue; // end of a sibling list
      const ab = abbrevs.get(code);
      if (!ab) throw new Error("bad abbrev code " + code);
      const at = {};
      for (const [name, form, ic] of ab.attrs) at[name] = this.readForm(b, form, cu, ic);
      if (ab.tag === 0x11 || ab.tag === 0x3c) {
        cu.compDir = this.attrStr(at[0x1b], cu);
        cu.name = this.attrStr(at[0x03], cu);
        cu.stmtList = at[0x10] ? at[0x10].v : undefined;
        cu.lowPc = at[0x11] ? this.attrAddr(at[0x11], cu) : 0;
        if (at[0x72]) cu.strOffBase = at[0x72].v;
        if (at[0x73]) cu.addrBase = at[0x73].v;
        if (at[0x74]) cu.rngBase = at[0x74].v;
        cu.top = at;
      }
      const rec = {};
      if (at[0x03]) rec.name = this.attrStr(at[0x03], cu);
      if (at[0x6e] || at[0x2007]) rec.link = this.attrStr(at[0x6e] || at[0x2007], cu);
      if (at[0x31]) rec.origin = this.refOff(at[0x31], cu);
      if (at[0x47]) rec.spec = this.refOff(at[0x47], cu);
      if (rec.name !== undefined || rec.origin !== undefined || rec.spec !== undefined || rec.link !== undefined)
        cu.dies.set(dieOff, rec);
      if (ab.tag === 0x2e || ab.tag === 0x1d) {
        for (const [lo, hi] of this.dieRanges(at, cu))
          if (hi > lo) cu.funcs.push({ lo, hi, die: dieOff, order: order++ });
      }
    }
    if (cu.top) cu.ranges = this.dieRanges(cu.top, cu);
  }

  readForm(b, form, cu, ic) {
    switch (form) {
      case 0x01: return { c: "addr", v: b.addr(cu.addrSize) };
      case 0x03: { const n = b.u16(); b.i += n; return { c: "block" }; }
      case 0x04: { const n = b.u32(); b.i += n; return { c: "block" }; }
      case 0x05: return { c: "const", v: b.u16() };
      case 0x06: return { c: "const", v: b.u32() };
      case 0x07: return { c: "const", v: b.u64() };
      case 0x08: return { c: "str", s: b.cstr() };
      case 0x09: case 0x18: { const n = b.uleb(); b.i += n; return { c: "block" }; }
      case 0x0a: { const n = b.u8(); b.i += n; return { c: "block" }; }
      case 0x0b: return { c: "const", v: b.u8() };
      case 0x0c: return { c: "flag", v: b.u8() };
      case 0x0d: return { c: "const", v: b.sleb() };
      case 0x0e: return { c: "strp", v: b.u32() };
      case 0x0f: return { c: "const", v: b.uleb() };
      case 0x10: return { c: "refaddr", v: cu.version === 2 ? b.addr(cu.addrSize) : b.u32() };
      case 0x11: return { c: "ref", v: b.u8() };
      case 0x12: return { c: "ref", v: b.u16() };
      case 0x13: return { c: "ref", v: b.u32() };
      case 0x14: return { c: "ref", v: b.u64() };
      case 0x15: return { c: "ref", v: b.uleb() };
      case 0x16: return this.readForm(b, b.uleb(), cu, ic);
      case 0x17: return { c: "secoff", v: b.u32() };
      case 0x19: return { c: "flag", v: 1 };
      case 0x1a: return { c: "strx", v: b.uleb() };
      case 0x1b: return { c: "addrx", v: b.uleb() };
      case 0x1c: b.i += 4; return { c: "sup" };
      case 0x1d: b.i += 4; return { c: "sup" };
      case 0x1e: b.i += 16; return { c: "block" };
      case 0x1f: return { c: "linestrp", v: b.u32() };
      case 0x20: b.i += 8; return { c: "sig" };
      case 0x21: return { c: "const", v: ic };
      case 0x22: return { c: "loclistx", v: b.uleb() };
      case 0x23: return { c: "rnglistx", v: b.uleb() };
      case 0x24: b.i += 8; return { c: "sup" };
      case 0x25: return { c: "strx", v: b.u8() };
      case 0x26: return { c: "strx", v: b.u16() };
      case 0x27: { const v = b.u16() | b.u8() << 16; return { c: "strx", v }; }
      case 0x28: return { c: "strx", v: b.u32() };
      case 0x29: return { c: "addrx", v: b.u8() };
      case 0x2a: return { c: "addrx", v: b.u16() };
      case 0x2b: { const v = b.u16() | b.u8() << 16; return { c: "addrx", v }; }
      case 0x2c: return { c: "addrx", v: b.u32() };
      default: throw new Error("unknown DWARF form 0x" + form.toString(16));
    }
  }

  attrStr(a, cu) {
    if (!a) return undefined;
    if (a.c === "str") return a.s;
    if (a.c === "strp") return cstrAt(this.str, a.v);
    if (a.c === "linestrp") return cstrAt(this.lineStr, a.v);
    if (a.c === "strx" && this.strOffsets) {
      const off = new Buf(this.strOffsets, (cu.strOffBase ?? 8) + a.v * 4).u32();
      return cstrAt(this.str, off);
    }
    return undefined;
  }

  attrAddr(a, cu) {
    if (a.c === "addrx") return this.addrx(a.v, cu);
    return a.v;
  }

  addrx(i, cu) {
    if (!this.addrSec) return 0;
    return new Buf(this.addrSec, (cu.addrBase ?? 8) + i * cu.addrSize).addr(cu.addrSize);
  }

  refOff(a, cu) { return a.c === "refaddr" ? a.v : a.c === "ref" ? cu.off + a.v : undefined; }

  dieRanges(at, cu) {
    if (at[0x11] && at[0x12]) {
      const lo = this.attrAddr(at[0x11], cu);
      const h = at[0x12];
      return [[lo, h.c === "const" ? lo + h.v : this.attrAddr(h, cu)]];
    }
    if (!at[0x55]) return [];
    const a = at[0x55];
    if (cu.version >= 5) {
      let off = a.v;
      if (a.c === "rnglistx") {
        const base = cu.rngBase ?? 12;
        off = base + new Buf(this.rnglists, base + a.v * 4).u32();
      }
      return this.rnglist(off, cu);
    }
    return this.rangeList(a.v, cu);
  }

  rnglist(off, cu) {
    const out = [];
    if (!this.rnglists) return out;
    const b = new Buf(this.rnglists, off), as = cu.addrSize;
    let base = cu.lowPc || 0;
    for (;;) {
      const k = b.u8();
      if (k === 0) break;
      if (k === 1) base = this.addrx(b.uleb(), cu);
      else if (k === 2) { const s = this.addrx(b.uleb(), cu); out.push([s, this.addrx(b.uleb(), cu)]); }
      else if (k === 3) { const s = this.addrx(b.uleb(), cu); out.push([s, s + b.uleb()]); }
      else if (k === 4) { const s = b.uleb(); out.push([base + s, base + b.uleb()]); }
      else if (k === 5) base = b.addr(as);
      else if (k === 6) { const s = b.addr(as); out.push([s, b.addr(as)]); }
      else if (k === 7) { const s = b.addr(as); out.push([s, s + b.uleb()]); }
      else throw new Error("bad rnglist entry");
    }
    return out;
  }

  rangeList(off, cu) {
    const out = [];
    if (!this.ranges) return out;
    const b = new Buf(this.ranges, off), as = cu.addrSize, max = as === 8 ? 2 ** 64 - 1 : 0xffffffff;
    let base = cu.lowPc || 0;
    for (;;) {
      const s = b.addr(as), e = b.addr(as);
      if (s === 0 && e === 0) break;
      if (s === max) { base = e; continue; }
      out.push([base + s, base + e]);
    }
    return out;
  }

  dieName(cu, off, depth) {
    if (depth > 8) return undefined;
    const d = cu.dies.get(off) || this.findDie(off);
    if (!d) return undefined;
    if (d.link) return d.link;
    if (d.name) return d.name;
    const next = d.origin ?? d.spec;
    if (next === undefined) return undefined;
    return this.dieName(cu, next, depth + 1);
  }

  findDie(off) {
    for (const cu of this.units) if (cu.dies.has(off)) return cu.dies.get(off);
    return undefined;
  }

  readLines(cu) {
    const b = new Buf(this.line, cu.stmtList);
    const len = b.u32();
    const end = b.i + len;
    const version = b.u16();
    let addrSize = cu.addrSize;
    if (version >= 5) { addrSize = b.u8(); b.u8(); }
    const hlen = b.u32();
    const progStart = b.i + hlen;
    const minInst = b.u8();
    if (version >= 4) b.u8();
    b.u8(); // default_is_stmt
    const lineBase = b.i8(), lineRange = b.u8(), opBase = b.u8();
    const stdLens = [0];
    for (let k = 1; k < opBase; k++) stdLens.push(b.u8());
    let dirs = [], files = [];
    if (version >= 5) {
      const entries = (cb) => {
        const fmts = [];
        const nf = b.u8();
        for (let k = 0; k < nf; k++) fmts.push([b.uleb(), b.uleb()]);
        const n = b.uleb(), out = [];
        for (let k = 0; k < n; k++) {
          const e = {};
          for (const [ct, form] of fmts) {
            const a = this.readForm(b, form, { ...cu, addrSize }, 0);
            if (ct === 1) e.path = this.attrStr(a, cu);
            else if (ct === 2) e.dir = a.v;
          }
          out.push(e);
        }
        return out;
      };
      dirs = entries().map((e) => e.path || "");
      files = entries().map((e) => ({ name: e.path || "", dir: e.dir || 0 }));
    } else {
      dirs.push(cu.compDir || "");
      for (let s; (s = b.cstr());) dirs.push(s);
      files.push({ name: cu.name || "", dir: 0 });
      for (let s; (s = b.cstr());) { const dir = b.uleb(); b.uleb(); b.uleb(); files.push({ name: s, dir }); }
    }
    const comp = cu.compDir || "";
    const fileName = (i) => {
      const f = files[i];
      if (!f) return "??";
      if (f.name.startsWith("/")) return f.name;
      const sub = f.dir ? dirs[f.dir] : version >= 5 ? dirs[0] : "";
      if (sub && sub.startsWith("/")) return joinpath(sub, f.name);
      return joinpath(comp, sub ? joinpath(sub, f.name) : f.name);
    };
    b.i = progStart;
    const seqs = [];
    let rows = [];
    let addr, file, line;
    const reset = () => { addr = 0; file = 1; line = 1; };
    const emit = (endSeq) => {
      const last = rows[rows.length - 1];
      // Keep only the last row at one address, as binutils does.
      if (last && last.addr === addr && !endSeq) rows.pop();
      rows.push({ addr, file, line, end: endSeq });
      if (endSeq) {
        if (rows.length > 1) seqs.push({ lo: rows[0].addr, hi: addr, rows });
        rows = [];
        reset();
      }
    };
    reset();
    while (b.i < end) {
      const op = b.u8();
      if (op >= opBase) {
        const adj = op - opBase;
        addr += Math.floor(adj / lineRange) * minInst;
        line += lineBase + (adj % lineRange);
        emit(false);
      } else if (op === 0) {
        const n = b.uleb(), next = b.i + n, sub = b.u8();
        if (sub === 1) emit(true);
        else if (sub === 2) addr = b.addr(n - 1);
        else if (sub === 3) { files.push({ name: b.cstr(), dir: b.uleb() }); }
        b.i = next;
      } else if (op === 1) emit(false);
      else if (op === 2) addr += b.uleb() * minInst;
      else if (op === 3) line += b.sleb();
      else if (op === 4) file = b.uleb();
      else if (op === 5) b.uleb();
      else if (op === 8) addr += Math.floor((255 - opBase) / lineRange) * minInst;
      else if (op === 9) addr += b.u16();
      else if (op === 12) b.uleb();
      else if (op !== 6 && op !== 7 && op !== 10 && op !== 11) for (let k = 0; k < stdLens[op]; k++) b.uleb();
    }
    return { seqs, fileName };
  }

  // addr2line -f: [function, "file:line"]; function is null when unknown.
  addr2line(addr) {
    if (this.cache.has(addr)) return this.cache.get(addr);
    let func = null, loc = "??:0", found = false;
    // addr2line only asks DWARF about addresses inside an allocated section;
    // functions dropped by --gc-sections still have debug info at address 0.
    const inSection = this.sections.some((x) => x.flags & 2 && x.addr <= addr && addr < x.addr + x.size);
    for (const cu of inSection ? this.unitsFor(addr) : []) {
      const l = cu.lines && lookupLine(cu.lines, addr);
      let best = null;
      for (const f of cu.funcs)
        if (f.lo <= addr && addr < f.hi && f.name !== undefined &&
            (!best || f.hi - f.lo < best.hi - best.lo || (f.hi - f.lo === best.hi - best.lo && f.order > best.order)))
          best = f;
      if (!l && !best) continue;
      loc = l || "??:?";
      if (best) func = best.name;
      found = true;
      break;
    }
    if (!func) {
      const s = this.symbolFor(addr);
      if (s) { func = s.name; if (!found) loc = `${s.file || "??"}:?`; }
    }
    const r = [func, loc];
    this.cache.set(addr, r);
    return r;
  }

  unitsFor(addr) {
    // Like binutils: a unit owns its ranges, its line sequences and its
    // functions' ranges (assembler units can overshoot by the Thumb bit).
    return this.units.filter((cu) => cu.ranges.some(([lo, hi]) => lo <= addr && addr < hi) ||
      (cu.lines && cu.lines.seqs.some((s) => s.lo <= addr && addr < s.hi)) ||
      cu.funcs.some((f) => f.lo <= addr && addr < f.hi));
  }

  // binutils elf_find_function: nearest symbol at or below addr in the
  // same section, no size limit;
  // on a tie one that covers addr wins, then FUNC over NOTYPE, then the smaller
  // if both cover it, else the first. Matches binutils 2.43 on the demos.
  symbolFor(addr) {
    const sec = this.sections.findIndex((s) => s.flags & 4 && s.addr <= addr && addr < s.addr + s.size);
    let best = null;
    for (const s of this.syms) {
      if (s.shndx !== sec || s.value > addr) continue;
      if (!best || s.value > best.value) { best = s; continue; }
      if (s.value < best.value) continue;
      const covers = (x) => addr < x.value + x.size;
      if (covers(s) !== covers(best)) { if (covers(s)) best = s; }
      else if (s.type !== best.type) { if (s.type === 2) best = s; }
      else if (covers(s) && s.size < best.size) best = s;
    }
    return best;
  }

  // lwsym's Resolver.lookup: [func, file:line relative to relTo] or null
  lookup(addr, relTo) {
    const [func, loc] = this.addr2line(addr);
    if (!func) return null;
    return [func, loc.startsWith("/") && relTo ? relpath(loc.replace(/:[^:]*$/, ""), relTo) + loc.match(/:[^:]*$/)[0] : loc];
  }

  // Build directory of the first unit with one: what lwsym's cwd was in make.
  compDir() {
    const cu = this.units.find((u) => u.compDir && u.name && !u.name.endsWith(".S") && !u.name.endsWith(".s"));
    return cu ? cu.compDir : (this.units.find((u) => u.compDir) || {}).compDir;
  }

  isReturn(w) {
    return this.riscv ? followsCallRv(this.code, w) : Boolean(w & 1) && followsCall(this.code, w);
  }
}

function lookupLine(lines, addr) {
  for (const s of lines.seqs) {
    if (addr < s.lo || addr >= s.hi) continue;
    let lo = 0, hi = s.rows.length - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (s.rows[mid].addr <= addr) lo = mid; else hi = mid - 1;
    }
    const r = s.rows[lo];
    return `${lines.fileName(r.file)}:${r.line}`;
  }
  return null;
}

function parseAbbrev(u8, off) {
  const b = new Buf(u8, off), m = new Map();
  for (;;) {
    const code = b.uleb();
    if (!code) break;
    const tag = b.uleb(), children = b.u8(), attrs = [];
    for (;;) {
      const name = b.uleb(), form = b.uleb();
      if (!name && !form) break;
      attrs.push([name, form, form === 0x21 ? b.sleb() : 0]);
    }
    m.set(code, { tag, children, attrs });
  }
  return m;
}

function codeAt(code, ret, back) {
  for (const [addr, bytes] of code)
    if (addr + back <= ret && ret <= addr + bytes.length) return [bytes, ret - addr];
  return null;
}

const h16 = (b, o) => b[o] | b[o + 1] << 8;

function followsCall(code, ret) {
  ret = (ret & ~1) >>> 0;
  const c = codeAt(code, ret, 4);
  if (!c) return false;
  const [b, o] = c;
  const hw1 = h16(b, o - 4), hw2 = h16(b, o - 2);
  if ((hw1 & 0xf800) === 0xf000 && (hw2 & 0xd000) === 0xd000) return true;
  return (hw2 & 0xff87) === 0x4780;
}

function followsCallRv(code, ret) {
  if (ret & 1) return false;
  const c = codeAt(code, ret, 2);
  if (!c) return false;
  const [b, o] = c;
  if (o >= 4) {
    const insn = (h16(b, o - 4) | h16(b, o - 2) << 16) >>> 0;
    if ((((insn & 0x7f) === 0x6f) || ((insn & 0x7f) === 0x67)) && (insn >>> 7 & 0x1f) === 1) return true;
  }
  const hw = h16(b, o - 2);
  if ((hw & 0xf07f) === 0x9002 && hw >> 7 & 0x1f) return true;
  return (hw & 0xe003) === 0x2001;
}

const callSite = (ret) => Math.max(((ret & ~1) >>> 0) - 2, 0);
const HEX = "0x[0-9a-fA-F]{8}";

// Port of lwsym.annotate(): lw_print text in, annotated text out.
export function annotate(text, elf, relTo = elf.compDir()) {
  const lines = text.split(/(?<=\n)/);
  const out = [];
  const stack = [];
  let inFault = false, badJump = null;
  const first = new Map(), cur = {};
  const look = (a) => elf.lookup(a, relTo);
  const remember = (kw) => {
    Object.assign(cur, kw);
    if (cur.sig && cur.where && !first.has(cur.sig)) first.set(cur.sig, `#${cur.seq}: ${cur.where}`);
  };
  const flushStack = () => {
    if (!stack.length) return;
    const frames = stack.filter((w) => elf.isReturn(w)).map((w) => [w, look(callSite(w))]).filter(([, h]) => h);
    if (frames.length) {
      out.push("   backtrace guess (return addresses found on the stack):\n");
      for (const [w, [func, loc]] of frames) out.push(`     ${hex8(w)}  ${func}  ${loc}\n`);
    }
    stack.length = 0;
  };
  const clear = () => { for (const k of Object.keys(cur)) delete cur[k]; };
  const regs = (pc, lr, pcName, lrName) => {
    const names = [look(pc), look(callSite(lr))];
    [[pcName, names[0]], [lrName, names[1]]].forEach(([label, hit]) => {
      const isPc = label === pcName;
      if (isPc && badJump) out.push(`   ${label} -> bad jump target (${badJump}), the caller is at ${lrName}\n`);
      else if (hit) out.push(`   ${label} -> ${hit[0]}  ${hit[1]}\n`);
      else if (isPc) out.push(`   ${label} -> not in this ELF (jump to a bad address?)\n`);
      if (hit && !(isPc && badJump) && !("where" in cur)) remember({ where: `${hit[0]}  ${hit[1]}` });
    });
  };
  const stackRow = new RegExp("^\\s+" + HEX);
  for (const line of lines) {
    const s = line.replace(/\n$/, "");
    if ((inFault && s.startsWith("   stack:")) || (inFault && stack.length && stackRow.test(s))) {
      out.push(line);
      for (const w of s.match(new RegExp(HEX, "g")) || []) stack.push(parseInt(w, 16));
      continue;
    }
    flushStack();
    out.push(line);
    let m = s.match(/^\[\s*\d+\] #\s*(\d+) !! /);
    if (m) { clear(); cur.seq = m[1]; }
    m = s.match(new RegExp("^\\s+sig=(" + HEX + ")$"));
    if (m) remember({ sig: m[1] });
    m = s.match(new RegExp("!! repeat sig=(" + HEX + ")"));
    if (m) {
      clear();
      out.push(`   same crash as ${first.get(m[1]) || "one no longer in the ring"}\n`);
      continue;
    }
    if (s.includes("!! ") && !s.includes("panic")) {
      inFault = true;
      m = s.match(/\b(INVSTATE|INVPC|IACCVIOL|IBUSERR)\b/);
      badJump = m ? m[1] : null;
      m = s.match(/trap (instruction (?:misaligned|access fault|page fault))/);
      if (m) badJump = m[1];
    } else if (/^\[\s*\d+\] #/.test(s)) inFault = false;

    m = s.match(new RegExp(`\\bpc=(${HEX}) lr=(${HEX}) sp=${HEX} xpsr=(${HEX})`));
    if (m) {
      const [pc, lr, xpsr] = m.slice(1).map((g) => parseInt(g, 16));
      if (!(xpsr & (1 << 24)) && !badJump) badJump = "T bit clear";
      regs(pc, lr, "pc", "lr");
      continue;
    }
    m = s.match(new RegExp(`\\bmepc=(${HEX}) ra=(${HEX}) sp=${HEX}`));
    if (m) {
      regs(parseInt(m[1], 16), parseInt(m[2], 16), "mepc", "ra");
      continue;
    }
    m = s.match(new RegExp(`!! panic pc=(${HEX})`));
    if (m) {
      const hit = look(callSite(parseInt(m[1], 16)));
      if (hit) {
        out.push(`   called from ${hit[0]}  ${hit[1]}\n`);
        remember({ where: `${hit[0]}  ${hit[1]}` });
      }
    }
  }
  flushStack();
  return out.join("");
}
