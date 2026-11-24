//! Runs one scenario through the Rust API and the same one through the C API
//! (tests/twin.c), then checks that both exports are byte-identical and that
//! tools/lwdecode.py turns the Rust export into the Rust-side lw_print text.
//! One #[test] on purpose: the ring is a single global.
use lastwords::{log, Kind, Level, Start};
use std::path::PathBuf;
use std::process::Command;
use std::sync::atomic::{AtomicU32, Ordering};

static T: AtomicU32 = AtomicU32::new(0);
extern "C" fn clock() -> u32 {
    T.fetch_add(7, Ordering::Relaxed) + 7
}

#[no_mangle]
extern "C" fn lw_test_store() {}

extern "C" {
    fn lw_test_fault(f: *const u32, len: usize);
}

// struct lw_fault_info as words: r0..xpsr, exc_return, sp, ipsr, cfsr, hfsr,
// mmfar, bfar, nstack, stack[2].
fn fault() {
    let f: [u32; 18] = [
        0, 0, 0, 0, 0, 0x0800_0a3d, 0x0800_0c42, 0x0100_0003, 0xffff_fff9, 0x2000_3f80, 3, 1 << 25, 1 << 30,
        0, 0, 2, 0xdead_beef, 0x0800_0a3d,
    ];
    unsafe { lw_test_fault(f.as_ptr(), f.len() * 4) }
}

#[test]
fn same_bytes_as_c() {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..");
    let out = PathBuf::from(env!("CARGO_TARGET_TMPDIR"));

    assert_eq!(lastwords::init(0x55), Start::Cold);
    lastwords::set_clock(Some(clock));
    lastwords::log(Level::Info, "sensor ok");
    log!(Level::Warn, "temp {} C, rail {:.2} V", 41, 3.3);
    lastwords::write(Level::Debug, Kind::Blob, &[1, 2, 3, 4, 0xfe, 0xff, 0, 9]);
    log!(Level::Error, "{}", "x".repeat(300));
    let mut longest = 0;
    lastwords::for_each(|e| {
        longest = longest.max(e.text().map_or(0, str::len));
        true
    });
    assert_eq!(longest, 255, "long line not cut to 255 bytes");
    fault();
    assert_eq!(lastwords::init(0), Start::Crashed);
    fault();
    for i in 0..40 {
        log!(Level::Info, "tick {} of {}", i, 40);
    }

    // Walk the ring: the second fault became a repeat with the same signature.
    let (mut sigs, mut kinds) = (Vec::new(), Vec::new());
    let n = lastwords::for_each(|e| {
        kinds.push(e.kind_raw());
        if e.signature() != 0 {
            sigs.push(e.signature());
        }
        true
    });
    assert!(kinds.contains(&(Kind::Repeat as u8)), "no repeat record: {kinds:?}");
    assert!(!sigs.is_empty() && sigs.iter().all(|&s| s == sigs[0]), "{sigs:x?}");
    let st = lastwords::stats();
    assert_eq!((st.boot, st.records), (1, n));

    let mut blob = Vec::new();
    let total = lastwords::export(|c| {
        assert!(c.len() <= 32);
        blob.extend_from_slice(c)
    });
    assert_eq!(total as usize, blob.len());
    let mut text = String::new();
    lastwords::print(|l| text.push_str(l));

    // C twin.
    let exe = out.join("twin");
    let ok = Command::new(std::env::var("CC").unwrap_or("cc".into()))
        .args(["-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-DLW_TEST_HOOK", "-I"])
        .arg(root.join("src"))
        .arg(root.join("rust/tests/twin.c"))
        .arg(root.join("src/lastwords.c"))
        .arg("-o")
        .arg(&exe)
        .status()
        .unwrap();
    assert!(ok.success());
    let (cb, ct) = (out.join("c.bin"), out.join("c.txt"));
    assert!(Command::new(&exe).arg(&cb).arg(&ct).status().unwrap().success());
    let c_blob = std::fs::read(&cb).unwrap();
    let c_text = std::fs::read_to_string(&ct).unwrap();
    assert_eq!(blob, c_blob, "export differs from the C twin");
    assert_eq!(text, c_text, "lw_print text differs from the C twin");

    // Host decoder.
    let rb = out.join("rust.bin");
    std::fs::write(&rb, &blob).unwrap();
    let dec = Command::new("python3").arg(root.join("tools/lwdecode.py")).arg(&rb).output().unwrap();
    assert!(dec.status.success(), "{}", String::from_utf8_lossy(&dec.stderr));
    assert_eq!(String::from_utf8(dec.stdout).unwrap(), text, "lwdecode.py output differs");

    println!("rust: {} records, export {} bytes, identical to C, lwdecode.py matches", n, blob.len());
    lastwords::clear();
    assert_eq!(lastwords::stats().records, 0);
}
