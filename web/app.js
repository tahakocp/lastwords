import { Elf, load, decode, render, annotate, BEGIN } from "./lwcore.js";

const $ = (id) => document.getElementById(id);
const state = { elf: null, elfName: "", logBytes: null, logName: "" };

const isElf = (u) => u[0] === 0x7f && u[1] === 0x45 && u[2] === 0x4c && u[3] === 0x46;

async function take(files) {
  for (const f of files) {
    const u = new Uint8Array(await f.arrayBuffer());
    if (isElf(u)) {
      try {
        state.elf = new Elf(u); state.elfName = f.name;
        $("base").value = state.elf.compDir() || "";
      } catch (e) { show(`${f.name}: ${e.message}`, true); return; }
    } else {
      state.logBytes = u; state.logName = f.name;
    }
  }
  run();
}

function latin1ToUtf8(s) {
  return new TextDecoder().decode(Uint8Array.from(s, (c) => c.charCodeAt(0)));
}

// lw_print text straight from a log, or rendered from the export(s) in it
function recordText(bytes) {
  const text = new TextDecoder().decode(bytes);
  const hasExport = text.includes(BEGIN) || (bytes[0] === 0x4c && bytes[1] === 0x57 && bytes[2] === 0x58 && bytes[3] === 0x31);
  if (!hasExport && /^\[\s*\d+\] #\s*\d+ /m.test(text)) return text;
  return load(bytes).map((b) => latin1ToUtf8(render(decode(b).entries))).join("");
}

function run() {
  const names = [state.elfName && `ELF: ${state.elfName}` + (state.elf && state.elf.riscv ? " (RISC-V)" : " (Arm)"),
    state.logName && `export: ${state.logName}`].filter(Boolean);
  $("files").textContent = names.join("   ");
  if (!state.logBytes) { show(state.elf ? "now drop the export or the log" : "waiting for an ELF and an export"); return; }
  let text;
  try { text = recordText(state.logBytes); } catch (e) { show(`${state.logName}: ${e.message}`, true); return; }
  if (!state.elf) { show(text + "\n(drop the ELF to get function names)"); return; }
  if (state.elf.dwarfError) text = `note: ${state.elf.dwarfError}; using the symbol table only\n` + text;
  const t0 = performance.now();
  const out = annotate(text, state.elf, $("base").value.trim() || undefined);
  show(out, false, performance.now() - t0);
}

function show(text, err, ms) {
  const pre = $("out");
  pre.textContent = "";
  if (err) { pre.innerHTML = ""; const s = document.createElement("span"); s.className = "err"; s.textContent = text; pre.append(s); return; }
  for (const line of text.split("\n")) {
    const s = document.createElement("span");
    if (/^\s+(pc|lr|mepc|ra) -> bad|not in this ELF/.test(line)) s.className = "bad";
    else if (/^\s+(pc|lr|mepc|ra) -> |^\s+called from|^\s+0x[0-9a-f]{8}  \S+  |same crash as/.test(line)) s.className = "sym";
    else if (/ !! /.test(line)) s.className = "hdr";
    s.textContent = line + "\n";
    pre.append(s);
  }
  if (ms !== undefined) pre.append(`\n(${ms.toFixed(1)} ms)`);
}

const drop = $("drop");
drop.addEventListener("click", () => $("pick").click());
$("pick").addEventListener("change", (e) => take(e.target.files));
drop.addEventListener("dragover", (e) => { e.preventDefault(); drop.classList.add("over"); });
drop.addEventListener("dragleave", () => drop.classList.remove("over"));
drop.addEventListener("drop", (e) => { e.preventDefault(); drop.classList.remove("over"); take(e.dataTransfer.files); });
$("base").addEventListener("change", run);
$("paste").addEventListener("input", (e) => {
  state.logBytes = new TextEncoder().encode(e.target.value); state.logName = "pasted text"; run();
});

// The Pages build drops a demo ELF and its UART log into demo/.
fetch("demo/demo-m3.elf", { method: "HEAD" }).then((r) => {
  if (!r.ok) return;
  $("demo").hidden = false;
  $("demo").addEventListener("click", async () => {
    const get = async (n) => new File([await (await fetch("demo/" + n)).arrayBuffer()], n);
    await take([await get("demo-m3.elf"), await get("run-m3.log")]);
  });
}).catch(() => {});
