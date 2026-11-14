use std::path::PathBuf;

fn main() {
    // tauri_build embeds the Windows .ico into the exe's resource table, but it
    // does NOT emit a cargo:rerun-if-changed for the icon files. So after you
    // swap icons/icon.ico, an incremental `cargo tauri build` relinks the exe
    // while reusing the STALE compiled resource (resource.lib) — the titlebar
    // and taskbar keep showing the old icon even though the new .ico is on disk.
    //
    // Tell cargo to re-run this build script (which recompiles the resource and
    // re-embeds the icon) whenever the icon set changes.
    println!("cargo:rerun-if-changed=icons/icon.ico");
    println!("cargo:rerun-if-changed=icons");

    let core_source = PathBuf::from("../core-source.json");
    println!("cargo:rerun-if-changed={}", core_source.display());
    let source: serde_json::Value = serde_json::from_str(
        &std::fs::read_to_string(core_source).expect("read core-source.json")
    ).expect("parse core-source.json");
    for (key, env) in [("version", "CALABI_BUNDLED_CORE_VERSION"), ("commit", "CALABI_BUNDLED_CORE_COMMIT")] {
        println!("cargo:rerun-if-env-changed={env}");
        let value = std::env::var(env).unwrap_or_else(|_| source[key].as_str().expect("core source field").to_owned());
        println!("cargo:rustc-env={env}={value}");
    }
    embed_daemon();

    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("macos")
        && std::env::var_os("CARGO_FEATURE_RDP_NATIVE_MACOS").is_some()
    {
        println!("cargo:rerun-if-changed=src/macos_rdp_view.m");
        cc::Build::new()
            .file("src/macos_rdp_view.m")
            .flag("-fobjc-arc")
            .compile("calabi_rdp_view");
        for framework in ["AppKit", "CoreGraphics", "QuartzCore"] {
            println!("cargo:rustc-link-lib=framework={framework}");
        }
    }

    tauri_build::try_build(tauri_build::Attributes::new().app_manifest(
        tauri_build::AppManifest::new().commands(&["product_info", "desktop_set_login_window", "files_action", "file_receipt_action", "tray_action", "tray_set_locale", "desktop_set_theme", "ssh_session", "ssh_read", "remote_workspace", "remote_frame"]),
    )).expect("build desktop permissions");
}

/// Bake the calabi daemon binary INTO the desktop exe so the shipped
/// calabi-desktop.exe is self-contained — no sidecar file, no separate install.
///
/// We copy the staged binary (binaries/calabi[.exe], placed there by
/// build-desktop.{ps1,sh}) into OUT_DIR/embedded-daemon, which the crate
/// include_bytes!()s. If no binary is staged (e.g. a plain `cargo build` on a
/// fresh checkout) we write an EMPTY placeholder so the crate still compiles;
/// at runtime an empty payload means "fall back to a sibling/PATH calabi".
///
/// A content signature is exported as CALABI_DAEMON_SIG so the runtime extracts
/// each distinct daemon build to its own filename (a new desktop release ships a
/// new daemon → new sig → fresh extract, never a stale cached copy).
fn embed_daemon() {
    // The daemon is NO LONGER baked into the desktop exe (the "spawn + embedded
    // removal" step): the installer delivers it — a .pkg on macOS,
    // and the NSIS installer ships calabi.exe on Windows — and the app ATTACHES to
    // the machine-wide system service. We still write an EMPTY OUT_DIR payload
    // (+ sig) so the include_bytes! in daemon.rs compiles; empty ⇒
    // extract_embedded_daemon returns None ⇒ resolve_binary falls back to a
    // sibling/PATH calabi, used only by DEBUG dev builds. This drops ~20MB of
    // duplicated daemon from the exe.
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("embedded-daemon");
    std::fs::write(&out, b"").expect("write OUT_DIR/embedded-daemon");
    println!("cargo:rustc-env=CALABI_DAEMON_SIG=none");
}
