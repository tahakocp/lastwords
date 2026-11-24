//! The C demo's story in Rust: five boots on QEMU mps2-an385. Boots 0-3
//! each die differently (bus fault, lastwords::fatal!, bad jump, the boot 0
//! bug again), boot 4 prints the ring and the export as hex and exits.
//! No runtime crates: the vector table and reset handler are below.
#![no_std]
#![no_main]

use core::arch::asm;
use core::ptr;
use lastwords::{log, Level};

extern "C" {
    static mut __data_start: u32;
    static mut __data_end: u32;
    static __data_load: u32;
    static mut __bss_start: u32;
    static mut __bss_end: u32;
    static __StackTop: u32;
    // From the C library (strong aliases of lw_fault_entry).
    fn HardFault_Handler();
    fn MemManage_Handler();
    fn BusFault_Handler();
    fn UsageFault_Handler();
}

#[repr(C)]
union Vector {
    handler: unsafe extern "C" fn(),
    reset: unsafe extern "C" fn() -> !,
    sp: *const u32,
    none: usize,
}
unsafe impl Sync for Vector {}

#[link_section = ".vectors"]
#[used]
static VECTORS: [Vector; 16] = [
    Vector { sp: &raw const __StackTop },
    Vector { reset: Reset_Handler },
    Vector { handler: idle },
    Vector { handler: HardFault_Handler },
    Vector { handler: MemManage_Handler },
    Vector { handler: BusFault_Handler },
    Vector { handler: UsageFault_Handler },
    Vector { none: 0 },
    Vector { none: 0 },
    Vector { none: 0 },
    Vector { none: 0 },
    Vector { handler: idle },
    Vector { handler: idle },
    Vector { none: 0 },
    Vector { handler: idle },
    Vector { handler: idle },
];

unsafe extern "C" fn idle() {
    loop {}
}

#[no_mangle]
unsafe extern "C" fn Reset_Handler() -> ! {
    let mut src = &raw const __data_load;
    let mut dst = &raw mut __data_start;
    while dst < &raw mut __data_end {
        ptr::write_volatile(dst, ptr::read(src));
        dst = dst.add(1);
        src = src.add(1);
    }
    let mut p = &raw mut __bss_start;
    while p < &raw mut __bss_end {
        ptr::write_volatile(p, 0);
        p = p.add(1);
    }
    main()
}

fn sh_call(op: u32, arg: *const u8) {
    unsafe { asm!("bkpt 0xab", inout("r0") op => _, in("r1") arg, options(nostack)) }
}

/// SYS_WRITE0 in chunks, since it wants a NUL.
fn say(s: &str) {
    for chunk in s.as_bytes().chunks(63) {
        let mut buf = [0u8; 64];
        buf[..chunk.len()].copy_from_slice(chunk);
        sh_call(0x04, buf.as_ptr());
    }
}

fn exit(ok: bool) -> ! {
    sh_call(0x18, (if ok { 0x20026usize } else { 0x20024 }) as *const u8);
    loop {}
}

static mut TICKS: u32 = 0;
extern "C" fn uptime() -> u32 {
    unsafe { ptr::read_volatile(&raw const TICKS) }
}

struct Config {
    dma_ctrl: *mut u32,
    channel: u32,
}

#[inline(never)]
fn config_apply(c: &Config) {
    // A plain store, so addr2line names config_apply rather than the
    // inlined core::ptr::write_volatile (lwsym does not pass -i yet).
    unsafe { *c.dma_ctrl = 1 << c.channel }
}

#[inline(never)]
fn pool_take(free_blocks: u32) {
    if free_blocks <= 4 {
        lastwords::fatal!("pool_take: free_blocks <= 4");
    }
}

#[inline(never)]
fn dispatch(handlers: &[usize; 4], id: usize) {
    let f: extern "C" fn() = unsafe { core::mem::transmute(ptr::read_volatile(&handlers[id])) };
    f()
}

#[panic_handler]
fn on_panic(_: &core::panic::PanicInfo) -> ! {
    say("rust panic\n");
    exit(false)
}

fn main() -> ! {
    let start = lastwords::init(0);
    lastwords::set_clock(Some(uptime));
    let boot = lastwords::stats().boot;
    let mut line = lastwords::Line::new();
    let _ = core::fmt::Write::write_fmt(&mut line, format_args!("boot {}: lastwords::init says {:?}\n", boot, start));
    say(core::str::from_utf8(line.as_bytes()).unwrap_or("?\n"));

    for i in 0..3 {
        unsafe { ptr::write_volatile(&raw mut TICKS, i) };
        log!(Level::Info, "boot {}, sensor sample {}", boot, i * 17);
    }

    match boot {
        0 | 3 => {
            lastwords::log(Level::Info, "applying DMA config");
            config_apply(&Config { dma_ctrl: 0x3000_0000 as *mut u32, channel: 3 });
        }
        1 => {
            log!(Level::Error, "pool low: {} blocks", 1);
            pool_take(1);
        }
        2 => {
            lastwords::log(Level::Info, "dispatching message 2");
            let handlers = [0usize, 0, 0x1000, 0]; // even address: Thumb bit missing
            dispatch(&handlers, 2);
        }
        _ => {
            say("---- recovered log ----\n");
            lastwords::print(say);
            let st = lastwords::stats();
            say(if st.records > 0 { "---- end ----\n" } else { "empty\n" });
            say("---- export ----\n");
            lastwords::export(|chunk| {
                const D: &[u8; 16] = b"0123456789abcdef";
                let mut buf = [0u8; 65];
                for (i, b) in chunk.iter().enumerate() {
                    buf[2 * i] = D[(b >> 4) as usize];
                    buf[2 * i + 1] = D[(b & 15) as usize];
                }
                buf[2 * chunk.len()] = b'\n';
                say(core::str::from_utf8(&buf[..2 * chunk.len() + 1]).unwrap());
            });
            say("---- end export ----\n");
            exit(boot == 4);
        }
    }
    say("expected a fault, got none\n");
    exit(false)
}
