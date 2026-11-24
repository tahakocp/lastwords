// web/lwcore.js against the Python tools and binutils addr2line, on the
// QEMU demo ELFs that `make qemu/riscv/armv8m` leave behind:
//   node tests/check_web.mjs examples/qemu/build/demo-m3.elf ...
// Each ELF needs its run-*.log next to it (the raw UART capture).
import { execFileSync } from "node:child_process";
import { readFileSync, existsSync } from "node:fs";
import path from "node:path";
import { Elf, load, decode, render, annotate } from "../web/lwcore.js";

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), "..");
let failed = 0;

function addr2lineTool(elf) {
  const names = elf.riscv ? ["riscv-none-elf-addr2line", "riscv64-unknown-elf-addr2line"] : ["arm-none-eabi-addr2line"];
  for (const n of names) {
    try { execFileSync("which", [n], { stdio: "ignore" }); return n; } catch { /* next */ }
  }
  throw new Error("no addr2line for " + (elf.riscv ? "RISC-V" : "Arm"));
}

function diff(label, want, got) {
  if (want === got) return true;
  const a = want.split("\n"), b = got.split("\n");
  const i = a.findIndex((l, k) => l !== b[k]);
  console.log(`FAIL ${label}, first difference at line ${i + 1}:\n  want: ${a[i]}\n  got:  ${b[i]}`);
  failed++;
  return false;
}

for (const elfPath of process.argv.slice(2)) {
  const elfFile = path.resolve(elfPath);
  const dir = path.dirname(path.dirname(elfFile)); // examples/<x>
  const tag = path.basename(elfFile, ".elf").replace(/^demo-/, "");
  const log = path.join(path.dirname(elfFile), `run-${tag}.log`);
  if (!existsSync(log)) throw new Error("missing " + log);
  const t0 = performance.now();
  const elf = new Elf(readFileSync(elfFile));
  const parseMs = performance.now() - t0;
  if (elf.dwarfError) { console.log(`FAIL ${tag}: ${elf.dwarfError}`); failed++; continue; }

  // 1. every halfword in the code sections, against addr2line -f
  const addrs = [];
  for (const [base, bytes] of elf.code) for (let o = 0; o < bytes.length; o += 2) addrs.push(base + o);
  const out = execFileSync(addr2lineTool(elf), ["-f", "-C", "-e", elfFile, ...addrs.map((a) => "0x" + a.toString(16))],
    { maxBuffer: 1 << 26 }).toString().split("\n");
  let bad = 0;
  addrs.forEach((a, i) => {
    const wantF = out[2 * i] === "??" ? null : out[2 * i];
    const wantL = out[2 * i + 1].replace(/ \(discriminator \d+\)/, "");
    const [f, l] = elf.addr2line(a);
    if (f !== wantF || (wantF && l !== wantL)) {
      if (bad++ < 5) console.log(`  ${tag} 0x${a.toString(16)}: want ${wantF} ${wantL}, got ${f} ${l}`);
    }
  });
  if (bad) failed++;
  console.log(`${bad ? "FAIL" : "ok  "} ${tag}: ${addrs.length - bad}/${addrs.length} addresses match addr2line (parse ${parseMs.toFixed(1)} ms)`);

  // 2. export decode against lwdecode.py
  const py = (code, args, opts = {}) => execFileSync("python3", ["-c", code, ...args], { maxBuffer: 1 << 24, ...opts }).toString("latin1");
  const want = py("import sys; sys.path.insert(0, sys.argv[1]); import lwdecode\n" +
    "for b in lwdecode.load(sys.argv[2]): sys.stdout.buffer.write(lwdecode.render(lwdecode.decode(b)[1]))",
    [path.join(root, "tools"), log]);
  const blobs = load(readFileSync(log));
  const got = blobs.map((b) => render(decode(b).entries)).join("");
  if (diff(`${tag} render`, want, got)) console.log(`ok   ${tag}: ${blobs.map((b) => b.length).join("+")} byte export decodes like lwdecode.py`);

  // 3. annotated UART log against lwsym.py (run from the example dir, as make does)
  const sym = (input) => execFileSync("python3", [path.join(root, "tools/lwsym.py"), elfFile],
    { cwd: dir, input, maxBuffer: 1 << 24 }).toString();
  const text = readFileSync(log, "utf8");
  if (diff(`${tag} annotate log`, sym(text), annotate(text, elf)))
    console.log(`ok   ${tag}: annotated log matches lwsym.py`);
  // 4. what the page does: export -> text -> annotation
  if (diff(`${tag} annotate export`, sym(want), annotate(got, elf)))
    console.log(`ok   ${tag}: annotated export matches lwsym.py`);
}
process.exit(failed ? 1 : 0);
