use std::env;
use std::path::PathBuf;

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let engine_dir = manifest_dir.join("engine");

    // Build the C++ engine using CMake
    let dst = cmake::build(engine_dir.to_str().unwrap());

    // Link the static library
    println!("cargo:rustc-link-search=native={}", dst.join("lib").display());
    println!("cargo:rustc-link-search=native={}", dst.display());
    println!("cargo:rustc-link-lib=static=rtl_engine");
    println!("cargo:rustc-link-lib=stdc++");

    // Re-run if engine sources change
    println!("cargo:rerun-if-changed=engine/src/");
    println!("cargo:rerun-if-changed=engine/include/");
    println!("cargo:rerun-if-changed=engine/CMakeLists.txt");
}
