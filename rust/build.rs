// Compiles ../src/lastwords.c (or csrc/ when packaged) with the C compiler
// that matches the Rust target and links it as a static library.
// No build dependencies: the cc crate would do the same with more knobs.
//
// Environment:
//   LASTWORDS_CC          compiler to use instead of the guessed one
//   LASTWORDS_CFLAGS      extra flags, space separated (e.g. -DLW_NO_DEDUP)
//   LASTWORDS_RING_SIZE   ring size in bytes (power of two, 256..65536), default 1024
use std::env;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let src = [manifest.join("csrc"), manifest.join("../src")]
        .into_iter()
        .find(|d| d.join("lastwords.c").exists())
        .expect("lastwords.c not found in csrc/ or ../src/");
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let target = env::var("TARGET").unwrap();

    let (cc, ar, mut flags) = toolchain(&target);
    let cc = env::var("LASTWORDS_CC").unwrap_or(cc);
    flags.extend(["-std=c99", "-Os", "-Wall", "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections"].map(String::from));
    if !target.contains("linux") && !target.contains("darwin") && !target.contains("windows") {
        // Bare metal: no vsnprintf. Rust formats, see lastwords::log!.
        flags.push("-DLW_NO_PRINTF".into());
    }
    if let Ok(n) = env::var("LASTWORDS_RING_SIZE") {
        flags.push(format!("-DLW_RING_SIZE={n}"));
    }
    if let Ok(extra) = env::var("LASTWORDS_CFLAGS") {
        flags.extend(extra.split_whitespace().map(String::from));
    }
    if env::var("CARGO_FEATURE_TEST_HOOK").is_ok() {
        flags.push("-DLW_TEST_HOOK".into());
    }

    let obj = out.join("lastwords.o");
    run(Command::new(&cc).args(&flags).arg("-I").arg(&src).arg("-c").arg(src.join("lastwords.c")).arg("-o").arg(&obj));
    let lib = out.join("liblastwords.a");
    let _ = std::fs::remove_file(&lib);
    run(Command::new(&ar).arg("crs").arg(&lib).arg(&obj));

    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=lastwords");
    println!("cargo:include={}", src.display());
    for f in ["lastwords.c", "lastwords.h"] {
        println!("cargo:rerun-if-changed={}", src.join(f).display());
    }
    for v in ["LASTWORDS_CC", "LASTWORDS_CFLAGS", "LASTWORDS_RING_SIZE"] {
        println!("cargo:rerun-if-env-changed={v}");
    }
}

fn toolchain(target: &str) -> (String, String, Vec<String>) {
    let s = |v: &[&str]| v.iter().map(|x| x.to_string()).collect::<Vec<_>>();
    let arm = |cpu: &str, fpu: &[&str]| {
        let mut f = s(&["-mthumb", &format!("-mcpu={cpu}")]);
        f.extend(s(fpu));
        ("arm-none-eabi-gcc".to_string(), "arm-none-eabi-ar".to_string(), f)
    };
    let hard = ["-mfloat-abi=hard", "-mfpu=fpv4-sp-d16"];
    match target {
        "thumbv6m-none-eabi" => arm("cortex-m0plus", &[]),
        "thumbv7m-none-eabi" => arm("cortex-m3", &[]),
        "thumbv7em-none-eabi" => arm("cortex-m4", &["-mfloat-abi=soft"]),
        "thumbv7em-none-eabihf" => arm("cortex-m4", &hard),
        "thumbv8m.base-none-eabi" => arm("cortex-m23", &[]),
        "thumbv8m.main-none-eabi" => arm("cortex-m33", &["-mfloat-abi=soft"]),
        "thumbv8m.main-none-eabihf" => arm("cortex-m33", &["-mfloat-abi=hard", "-mfpu=fpv5-sp-d16"]),
        t if t.starts_with("riscv32") => {
            let isa = t.split('-').next().unwrap().trim_start_matches("riscv32");
            ("riscv-none-elf-gcc".into(), "riscv-none-elf-ar".into(), vec![format!("-march=rv32{isa}_zicsr"), "-mabi=ilp32".into()])
        }
        _ => ("cc".into(), "ar".into(), Vec::new()),
    }
}

fn run(cmd: &mut Command) {
    let st = cmd.status().unwrap_or_else(|e| panic!("{cmd:?}: {e}"));
    assert!(st.success(), "{cmd:?} failed");
}
