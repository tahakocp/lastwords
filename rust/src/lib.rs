//! Rust bindings for [lastwords](https://github.com/tahakocp/lastwords), a
//! crash flight recorder for Cortex-M and RV32.
//!
//! This is a binding, not a port: `build.rs` compiles the C library for the
//! target and links it, so records, exports and fault capture are the C
//! code's own. What Rust adds is a safe API and [`log!`], which formats with
//! `core::fmt` into a 255-byte line instead of `vsnprintf`.
//!
//! The C library takes no lock on hosted targets. On Cortex-M it masks
//! interrupts around each write; on a host, call it from one thread.
#![no_std]

use core::fmt;

/// Longest payload of one record; longer lines are cut to this.
pub const MAX_PAYLOAD: usize = 255;

/// Version of the C library this crate was written against.
pub const C_VERSION: &str = "0.6.0";

#[repr(C)]
struct RawEntry {
    ty: u8,
    level: u8,
    seq: u16,
    time: u32,
    len: u32,
    data: [u8; MAX_PAYLOAD],
}

#[repr(C)]
#[derive(Default)]
struct RawStats {
    boot: u32,
    records: u32,
    bytes_used: u32,
}

type VisitFn = unsafe extern "C" fn(*const RawEntry, *mut core::ffi::c_void) -> i32;
type OutFn = unsafe extern "C" fn(*const u8, *mut core::ffi::c_void);
type SinkFn = unsafe extern "C" fn(*const u8, usize, *mut core::ffi::c_void);

extern "C" {
    fn lw_init(reset_cause: u32) -> u32;
    fn lw_set_clock(now: Option<extern "C" fn() -> u32>);
    fn lw_write(level: u32, ty: u32, data: *const u8, len: usize);
    fn lw_panic(msg: *const u8) -> !;
    fn lw_foreach(f: VisitFn, ctx: *mut core::ffi::c_void) -> u32;
    fn lw_print(f: OutFn, ctx: *mut core::ffi::c_void);
    fn lw_get_stats(st: *mut RawStats);
    fn lw_signature(e: *const RawEntry) -> u32;
    fn lw_export(f: SinkFn, ctx: *mut core::ffi::c_void) -> u32;
    fn lw_clear();
    fn lw_region(size: *mut usize) -> *const u8;
}

/// What [`init`] found in RAM.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Start {
    /// No valid ring: power-on, or RAM was lost.
    Cold,
    /// Ring recovered; the previous boot ended normally or by an external reset.
    Warm,
    /// Ring recovered and the previous boot ended in a fault or panic.
    Crashed,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
#[repr(u8)]
pub enum Level {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
    Fatal = 4,
}

/// Record types, as in `enum lw_type`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Kind {
    Text = 0,
    Boot = 1,
    Fault = 2,
    Panic = 3,
    Blob = 4,
    Repeat = 5,
    Trap = 6,
}

impl Level {
    fn from_u8(v: u8) -> Level {
        match v {
            0 => Level::Debug,
            1 => Level::Info,
            2 => Level::Warn,
            3 => Level::Error,
            _ => Level::Fatal,
        }
    }
}

/// One record, borrowed for the duration of a [`for_each`] callback.
pub struct Entry<'a> {
    raw: &'a RawEntry,
}

impl<'a> Entry<'a> {
    /// Raw type number; unknown numbers come from newer firmware.
    pub fn kind_raw(&self) -> u8 {
        self.raw.ty
    }
    pub fn kind(&self) -> Option<Kind> {
        Some(match self.raw.ty {
            0 => Kind::Text,
            1 => Kind::Boot,
            2 => Kind::Fault,
            3 => Kind::Panic,
            4 => Kind::Blob,
            5 => Kind::Repeat,
            6 => Kind::Trap,
            _ => return None,
        })
    }
    pub fn level(&self) -> Level {
        Level::from_u8(self.raw.level)
    }
    pub fn seq(&self) -> u16 {
        self.raw.seq
    }
    pub fn time(&self) -> u32 {
        self.raw.time
    }
    pub fn data(&self) -> &'a [u8] {
        &self.raw.data[..(self.raw.len as usize).min(MAX_PAYLOAD)]
    }
    /// Text of a `Text` record (lossy only in that invalid UTF-8 gives `None`).
    pub fn text(&self) -> Option<&'a str> {
        match self.kind() {
            Some(Kind::Text) => core::str::from_utf8(self.data()).ok(),
            _ => None,
        }
    }
    /// Crash signature (FNV-1a, see the C header); 0 for non-crash records.
    pub fn signature(&self) -> u32 {
        unsafe { lw_signature(self.raw) }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Stats {
    pub boot: u32,
    pub records: u32,
    pub bytes_used: u32,
}

/// Call once, early, before the first log call.
pub fn init(reset_cause: u32) -> Start {
    match unsafe { lw_init(reset_cause) } {
        0 => Start::Cold,
        1 => Start::Warm,
        _ => Start::Crashed,
    }
}

/// Clock stored with each record (any unit). `None` stores 0.
pub fn set_clock(now: Option<extern "C" fn() -> u32>) {
    unsafe { lw_set_clock(now) }
}

/// Append a text record. Lines over 255 bytes are cut.
pub fn log(level: Level, text: &str) {
    write(level, Kind::Text, text.as_bytes());
}

/// Append a record of any type with a raw payload (cut to 255 bytes).
pub fn write(level: Level, kind: Kind, data: &[u8]) {
    let n = data.len().min(MAX_PAYLOAD);
    unsafe { lw_write(level as u32, kind as u32, data.as_ptr(), n) }
}

/// Record a panic with the caller's address and reset through `lw_reboot`.
///
/// Inlined so the address the C side records is in the calling function,
/// which is what `lwsym` and the crash signature need. The message is cut
/// at 127 bytes and at the first NUL.
#[inline(always)]
pub fn panic(msg: &str) -> ! {
    let mut buf = [0u8; 128];
    let n = msg.len().min(buf.len() - 1);
    buf[..n].copy_from_slice(&msg.as_bytes()[..n]);
    unsafe { lw_panic(buf.as_ptr()) }
}

/// Walk the ring from oldest to newest. Return `false` to stop.
/// Returns the number of records visited.
pub fn for_each<F: FnMut(&Entry) -> bool>(mut f: F) -> u32 {
    unsafe extern "C" fn tramp<F: FnMut(&Entry) -> bool>(e: *const RawEntry, ctx: *mut core::ffi::c_void) -> i32 {
        let f = &mut *(ctx as *mut F);
        (!f(&Entry { raw: &*e })) as i32
    }
    unsafe { lw_foreach(tramp::<F>, &mut f as *mut F as *mut _) }
}

/// Human-readable dump, one line at a time, each ending in `'\n'`.
pub fn print<F: FnMut(&str)>(mut f: F) {
    unsafe extern "C" fn tramp<F: FnMut(&str)>(line: *const u8, ctx: *mut core::ffi::c_void) {
        let f = &mut *(ctx as *mut F);
        let mut n = 0;
        while *line.add(n) != 0 {
            n += 1;
        }
        let bytes = core::slice::from_raw_parts(line, n);
        // lw_print only emits ASCII plus the text of records; keep invalid
        // UTF-8 visible instead of dropping the line.
        match core::str::from_utf8(bytes) {
            Ok(s) => f(s),
            Err(e) => {
                f(core::str::from_utf8_unchecked(&bytes[..e.valid_up_to()]));
                f("\u{fffd}...\n");
            }
        }
    }
    unsafe { lw_print(tramp::<F>, &mut f as *mut F as *mut _) }
}

/// Stream the ring in the compact export format to `sink`, in chunks of up
/// to 32 bytes. Returns the number of bytes produced.
pub fn export<F: FnMut(&[u8])>(mut sink: F) -> u32 {
    unsafe extern "C" fn tramp<F: FnMut(&[u8])>(d: *const u8, n: usize, ctx: *mut core::ffi::c_void) {
        let f = &mut *(ctx as *mut F);
        f(core::slice::from_raw_parts(d, n))
    }
    unsafe { lw_export(tramp::<F>, &mut sink as *mut F as *mut _) }
}

pub fn stats() -> Stats {
    let mut s = RawStats::default();
    unsafe { lw_get_stats(&mut s) };
    Stats { boot: s.boot, records: s.records, bytes_used: s.bytes_used }
}

/// Forget everything, e.g. after the log was uploaded.
pub fn clear() {
    unsafe { lw_clear() }
}

/// The raw region (header + ring), for copying to flash. It changes under
/// you if anything logs while you hold it.
pub fn region() -> *const [u8] {
    let mut n = 0usize;
    let p = unsafe { lw_region(&mut n) };
    core::ptr::slice_from_raw_parts(p, n)
}

/// A 255-byte line for [`log!`]; extra output is dropped, as `lw_logf` does.
pub struct Line {
    buf: [u8; MAX_PAYLOAD],
    len: usize,
}

impl Line {
    pub const fn new() -> Line {
        Line { buf: [0; MAX_PAYLOAD], len: 0 }
    }
    pub fn as_bytes(&self) -> &[u8] {
        &self.buf[..self.len]
    }
    pub fn emit(&self, level: Level) {
        write(level, Kind::Text, self.as_bytes());
    }
}

impl Default for Line {
    fn default() -> Self {
        Line::new()
    }
}

impl fmt::Write for Line {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        let n = s.len().min(MAX_PAYLOAD - self.len);
        self.buf[self.len..self.len + n].copy_from_slice(&s.as_bytes()[..n]);
        self.len += n;
        Ok(())
    }
}

/// `log!(Level::Info, "x = {}", x)`: format with `core::fmt` into a stack
/// line of 255 bytes and append it. Uses about 260 bytes of stack.
#[macro_export]
macro_rules! log {
    ($level:expr, $($arg:tt)*) => {{
        let mut line = $crate::Line::new();
        let _ = core::fmt::Write::write_fmt(&mut line, format_args!($($arg)*));
        line.emit($level);
    }};
}
