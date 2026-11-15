//! Bounded, metadata-only diagnostics. Never collect URLs, command lines or pixels.
use serde_json::{json, Value};
use std::{
    collections::HashSet,
    fs::OpenOptions,
    io::Write,
    path::{Path, PathBuf},
    sync::{mpsc, OnceLock},
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};
use windows_sys::Win32::{
    Foundation::{CloseHandle, HANDLE, INVALID_HANDLE_VALUE},
    System::{
        Diagnostics::ToolHelp::{
            CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W,
            TH32CS_SNAPPROCESS,
        },
        ProcessStatus::{
            GetPerformanceInfo, GetProcessMemoryInfo, PERFORMANCE_INFORMATION,
            PROCESS_MEMORY_COUNTERS_EX,
        },
        Threading::{
            GetProcessHandleCount, GetProcessTimes, OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION,
        },
    },
};

const INTERVAL: Duration = Duration::from_secs(60);
const MAX_LOG: u64 = 4 * 1024 * 1024;
static EVENTS: OnceLock<mpsc::SyncSender<Value>> = OnceLock::new();

fn emit(value: Value) {
    if let Some(sender) = EVENTS.get() {
        // Never block a WebView callback, including during memory pressure.
        let _ = sender.try_send(value);
    }
}

fn append(path: &Path, value: &Value, limit: u64) -> std::io::Result<()> {
    let mut line = serde_json::to_vec(value)?;
    line.push(b'\n');
    if line.len() as u64 > limit {
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            "diagnostic entry exceeds log limit",
        ));
    }
    if path.metadata().map(|m| m.len()).unwrap_or(0) + line.len() as u64 > limit {
        let backup = path.with_extension("jsonl.1");
        match std::fs::remove_file(&backup) {
            Ok(()) => (),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => (),
            Err(e) => return Err(e),
        }
        if path.exists() {
            std::fs::rename(path, backup)?;
        }
    }
    let mut file = OpenOptions::new().create(true).append(true).open(path)?;
    file.write_all(&line)?;
    file.flush()
}

fn write(path: &Path, mut value: Value) {
    value["timestamp_ms"] = json!(SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64);
    value["desktop_pid"] = json!(std::process::id());
    value["version"] = json!(env!("CARGO_PKG_VERSION"));
    if let Err(error) = append(path, &value, MAX_LOG) {
        // Once per worker lifetime; a full/read-only disk must not flood the other log.
        static REPORTED: OnceLock<()> = OnceLock::new();
        if REPORTED.set(()).is_ok() {
            log::warn!("desktop diagnostics write failed: {error}");
        }
    }
}

fn start() {
    let Some(base) = std::env::var_os("LOCALAPPDATA") else {
        log::warn!("desktop diagnostics unavailable: LOCALAPPDATA is missing");
        return;
    };
    let dir = PathBuf::from(base).join("Calabi/Logs");
    if let Err(error) = std::fs::create_dir_all(&dir) {
        log::warn!("desktop diagnostics directory unavailable: {error}");
        return;
    }
    let path = dir.join("desktop-diagnostics.jsonl");
    let (sender, receiver) = mpsc::sync_channel(64);
    if EVENTS.set(sender).is_err() {
        return;
    }
    log::info!(
        "desktop diagnostics: {} (60s sampling, 2 x 4 MiB)",
        path.display()
    );
    if let Err(error) = std::thread::Builder::new()
        .name("desktop-diagnostics".into())
        .spawn(move || {
            write(
                &path,
                json!({"event":"boot", "core_commit":env!("CALABI_BUNDLED_CORE_COMMIT")}),
            );
            write(&path, memory_snapshot());
            let mut next = Instant::now() + INTERVAL;
            loop {
                match receiver.recv_timeout(next.saturating_duration_since(Instant::now())) {
                    Ok(event) => {
                        let failed = event["event"] == "webview_process_failed";
                        write(&path, event);
                        if failed {
                            write(&path, memory_snapshot());
                        }
                    }
                    Err(mpsc::RecvTimeoutError::Disconnected) => break,
                    Err(mpsc::RecvTimeoutError::Timeout) => (),
                }
                if Instant::now() >= next {
                    write(&path, memory_snapshot());
                    next = Instant::now() + INTERVAL;
                }
            }
        })
    {
        log::warn!("desktop diagnostics thread unavailable: {error}");
    }
}

struct Handle(HANDLE);
impl Drop for Handle {
    fn drop(&mut self) {
        unsafe {
            CloseHandle(self.0);
        }
    }
}

fn process_memory(pid: u32) -> Value {
    unsafe {
        let raw = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
        if raw.is_null() {
            return json!({"query_error":std::io::Error::last_os_error().raw_os_error()});
        }
        let handle = Handle(raw);
        let mut memory = PROCESS_MEMORY_COUNTERS_EX::default();
        memory.cb = std::mem::size_of_val(&memory) as u32;
        let mut value = json!({});
        if GetProcessMemoryInfo(
            handle.0,
            (&mut memory as *mut PROCESS_MEMORY_COUNTERS_EX).cast(),
            memory.cb,
        ) != 0
        {
            value["private_bytes"] = json!(memory.PrivateUsage);
            value["working_set_bytes"] = json!(memory.WorkingSetSize);
        } else {
            value["memory_error"] = json!(std::io::Error::last_os_error().raw_os_error());
        }
        let mut handles = 0;
        if GetProcessHandleCount(handle.0, &mut handles) != 0 {
            value["handles"] = json!(handles);
        }
        let (mut created, mut exited, mut kernel, mut user) = Default::default();
        if GetProcessTimes(handle.0, &mut created, &mut exited, &mut kernel, &mut user) != 0 {
            value["created_filetime"] =
                json!(((created.dwHighDateTime as u64) << 32) | created.dwLowDateTime as u64);
        }
        value
    }
}

#[derive(Clone)]
struct Process {
    pid: u32,
    parent: u32,
    name: String,
    threads: u32,
}

fn selected_processes(processes: &[Process], root: u32) -> HashSet<u32> {
    let mut ids = HashSet::from([root]);
    // The installed service and user file agent may not descend from the GUI.
    ids.extend(
        processes
            .iter()
            .filter(|p| p.name.eq_ignore_ascii_case("calabi.exe"))
            .map(|p| p.pid),
    );
    loop {
        let before = ids.len();
        for p in processes {
            if ids.contains(&p.parent) {
                ids.insert(p.pid);
            }
        }
        if ids.len() == before {
            return ids;
        }
    }
}

fn memory_snapshot() -> Value {
    let mut value = json!({"event":"memory_sample"});
    match crate::windows_file_agent::current_session_active() {
        Ok(active) => value["windows_session_active"] = json!(active),
        Err(error) => value["session_query_error"] = json!(error.raw_os_error()),
    }
    unsafe {
        let mut system = PERFORMANCE_INFORMATION::default();
        system.cb = std::mem::size_of_val(&system) as u32;
        if GetPerformanceInfo(&mut system, system.cb) != 0 {
            value["system"] = json!({
                "commit_bytes":system.CommitTotal.saturating_mul(system.PageSize),
                "commit_limit_bytes":system.CommitLimit.saturating_mul(system.PageSize),
                "physical_available_bytes":system.PhysicalAvailable.saturating_mul(system.PageSize),
                "physical_total_bytes":system.PhysicalTotal.saturating_mul(system.PageSize),
                "process_count":system.ProcessCount,
            });
        } else {
            value["system_error"] = json!(std::io::Error::last_os_error().raw_os_error());
        }
        let raw = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if raw == INVALID_HANDLE_VALUE {
            value["snapshot_error"] = json!(std::io::Error::last_os_error().raw_os_error());
            return value;
        }
        let snapshot = Handle(raw);
        let mut entry = PROCESSENTRY32W::default();
        entry.dwSize = std::mem::size_of_val(&entry) as u32;
        let mut processes = Vec::new();
        let mut found = Process32FirstW(snapshot.0, &mut entry);
        while found != 0 {
            let end = entry
                .szExeFile
                .iter()
                .position(|c| *c == 0)
                .unwrap_or(entry.szExeFile.len());
            processes.push(Process {
                pid: entry.th32ProcessID,
                parent: entry.th32ParentProcessID,
                name: String::from_utf16_lossy(&entry.szExeFile[..end]),
                threads: entry.cntThreads,
            });
            found = Process32NextW(snapshot.0, &mut entry);
        }
        let ids = selected_processes(&processes, std::process::id());
        value["processes"] = Value::Array(
            processes
                .iter()
                .filter(|p| ids.contains(&p.pid))
                .map(|p| {
                    let mut data = process_memory(p.pid);
                    data["pid"] = json!(p.pid);
                    data["parent_pid"] = json!(p.parent);
                    data["name"] = json!(p.name);
                    data["threads"] = json!(p.threads);
                    data
                })
                .collect(),
        );
    }
    value
}

pub fn init() -> tauri::plugin::TauriPlugin<tauri::Wry> {
    tauri::plugin::Builder::new("desktop-diagnostics")
        .setup(|_, _| {
            start();
            Ok(())
        })
        .on_webview_ready(hook_webview)
        .on_event(|_, event| {
            if let tauri::RunEvent::WindowEvent {
                label,
                event: tauri::WindowEvent::Destroyed,
                ..
            } = event
            {
                emit(json!({"event":"window_destroyed", "window":label}));
            }
        })
        .build()
}

fn hook_webview(webview: tauri::Webview) {
    use webview2_com::{take_pwstr, Microsoft::Web::WebView2::Win32::*, ProcessFailedEventHandler};
    use windows::core::{Interface, PWSTR};
    let label = webview.label().to_owned();
    let result = webview.with_webview(move |platform| unsafe {
                let attach = || -> windows::core::Result<()> {
                    let core = platform.controller().CoreWebView2()?;
                    let mut pid = 0;
                    core.BrowserProcessId(&mut pid)?;
                    let mut version = PWSTR::null();
                    platform.environment().BrowserVersionString(&mut version)?;
                    let version = take_pwstr(version);
                    let event_label = label.clone();
                    let event_version = version.clone();
                    let handler = ProcessFailedEventHandler::create(Box::new(move |sender, args| {
                        let mut event = json!({"event":"webview_process_failed", "window":event_label,
                            "runtime":event_version, "browser_pid":pid});
                        if let Some(sender) = sender {
                            let mut pid = 0;
                            if sender.BrowserProcessId(&mut pid).is_ok() { event["browser_pid"] = json!(pid); }
                        }
                        if let Some(args) = args {
                            let mut kind = COREWEBVIEW2_PROCESS_FAILED_KIND::default();
                            if args.ProcessFailedKind(&mut kind).is_ok() { event["kind"] = json!(kind.0); }
                            if let Ok(details) = args.cast::<ICoreWebView2ProcessFailedEventArgs2>() {
                                let mut reason = COREWEBVIEW2_PROCESS_FAILED_REASON::default();
                                let mut exit = 0;
                                if details.Reason(&mut reason).is_ok() {
                                    event["reason"] = json!(reason.0);
                                    event["reason_name"] = json!(match reason {
                                        COREWEBVIEW2_PROCESS_FAILED_REASON_UNEXPECTED => "unexpected",
                                        COREWEBVIEW2_PROCESS_FAILED_REASON_UNRESPONSIVE => "unresponsive",
                                        COREWEBVIEW2_PROCESS_FAILED_REASON_TERMINATED => "terminated",
                                        COREWEBVIEW2_PROCESS_FAILED_REASON_CRASHED => "crashed",
                                        COREWEBVIEW2_PROCESS_FAILED_REASON_LAUNCH_FAILED => "launch_failed",
                                        COREWEBVIEW2_PROCESS_FAILED_REASON_OUT_OF_MEMORY => "out_of_memory",
                                        COREWEBVIEW2_PROCESS_FAILED_REASON_PROFILE_DELETED => "profile_deleted",
                                        _ => "unknown",
                                    });
                                }
                                if details.ExitCode(&mut exit).is_ok() { event["exit_code"] = json!(format!("0x{:08x}", exit as u32)); }
                            }
                        }
                        // Only owned strings/numbers cross to the worker. No COM object/window
                        // is captured by the handler, so destroying the WebView releases it.
                        emit(event);
                        Ok(())
                    }));
                    let mut token = 0;
                    core.add_ProcessFailed(&handler, &mut token)?;
                    emit(json!({"event":"webview_ready", "window":label, "browser_pid":pid, "runtime":version}));
                    Ok(())
                };
                if let Err(error) = attach() {
                    log::warn!("WebView diagnostics hook failed: window={label} error={error}");
                }
            });
    if let Err(error) = result {
        log::warn!("WebView diagnostics dispatch failed: {error}");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Run alone: creates an isolated, hidden WebView and intentionally crashes only
    // its renderer. No installed application, daemon or user profile is opened.
    #[test]
    #[ignore = "requires installed WebView2 and an interactive Windows desktop"]
    fn native_webview_failure_is_observed() {
        use std::sync::{Arc, Mutex};
        use tauri::Manager;
        // WebView2 may retain environment handles until process exit. The parent
        // owns the temporary directory and cleans it after the native host exits.
        let Some(directory) = std::env::var_os("CALABI_WEBVIEW_DIAGNOSTICS_FIXTURE") else {
            let directory = tempfile::Builder::new()
                .prefix("calabi-webview-diagnostics-fixture-")
                .tempdir()
                .unwrap();
            let output = std::process::Command::new(std::env::current_exe().unwrap())
                .args([
                    "diagnostics::tests::native_webview_failure_is_observed",
                    "--ignored",
                    "--exact",
                    "--nocapture",
                    "--test-threads=1",
                ])
                .env("CALABI_WEBVIEW_DIAGNOSTICS_FIXTURE", directory.path())
                .output()
                .unwrap();
            println!("{}", String::from_utf8_lossy(&output.stdout));
            assert!(
                output.status.success(),
                "native fixture failed: {}",
                String::from_utf8_lossy(&output.stderr)
            );
            let deadline = Instant::now() + Duration::from_secs(5);
            while let Err(error) = std::fs::remove_dir_all(directory.path()) {
                if error.kind() == std::io::ErrorKind::NotFound {
                    break;
                }
                assert!(
                    Instant::now() < deadline,
                    "fixture cleanup failed after process exit: {error}"
                );
                std::thread::sleep(Duration::from_millis(100));
            }
            return;
        };
        let data = PathBuf::from(directory).join("webview");
        let (sender, receiver) = mpsc::sync_channel(64);
        assert!(EVENTS.set(sender).is_ok(), "run this test alone");
        let outcome = Arc::new(Mutex::new(None));
        let result = outcome.clone();
        let worker = Arc::new(Mutex::new(None));
        let worker_setup = worker.clone();
        let app = tauri::Builder::default()
            .any_thread()
            .plugin(
                tauri::plugin::Builder::<tauri::Wry, ()>::new("diagnostics-fixture")
                    .on_webview_ready(hook_webview)
                    .build(),
            )
            .setup(move |app| {
                tauri::WebviewWindowBuilder::new(
                    app,
                    "diagnostics-fixture",
                    tauri::WebviewUrl::External("about:blank".parse().unwrap()),
                )
                .data_directory(data)
                .visible(false)
                .build()?;
                let handle = app.handle().clone();
                *worker_setup.lock().unwrap() = Some(std::thread::spawn(move || {
                    let deadline = Instant::now() + Duration::from_secs(30);
                    while let Ok(event) =
                        receiver.recv_timeout(deadline.saturating_duration_since(Instant::now()))
                    {
                        if event["event"] == "webview_ready" {
                            handle
                                .get_webview_window("diagnostics-fixture")
                                .unwrap()
                                .with_webview(|platform| unsafe {
                                    platform
                                        .controller()
                                        .CoreWebView2()
                                        .unwrap()
                                        .Navigate(windows::core::w!("chrome://crash"))
                                        .unwrap();
                                })
                                .unwrap();
                        }
                        if event["event"] == "webview_process_failed" {
                            *result.lock().unwrap() = Some(event);
                            break;
                        }
                        if Instant::now() >= deadline {
                            break;
                        }
                    }
                    handle.exit(0);
                }));
                Ok(())
            })
            .build(tauri::generate_context!())
            .unwrap();
        assert_eq!(app.run_return(|_, _| {}), 0);
        worker.lock().unwrap().take().unwrap().join().unwrap();
        let result = outcome.lock().unwrap();
        let event = result
            .as_ref()
            .expect("native ProcessFailed callback timed out");
        assert_eq!(event["kind"], 1); // render process exited
        assert_eq!(event["reason_name"], "crashed");
        assert!(event["runtime"].as_str().unwrap().contains('.'));
        assert!(event["browser_pid"].as_u64().unwrap() > 0);
        assert!(event["exit_code"].as_str().unwrap().starts_with("0x"));
        println!("isolated native WebView callback: {event}");
    }

    #[test]
    fn sampling_covers_nested_webviews_and_service_without_other_browsers() {
        let p = |pid, parent, name: &str| Process {
            pid,
            parent,
            name: name.into(),
            threads: 1,
        };
        let processes = [
            p(30, 20, "msedgewebview2.exe"),
            p(20, 10, "msedgewebview2.exe"),
            p(10, 1, "calabi-desktop.exe"),
            p(40, 1, "calabi.exe"),
            p(50, 40, "powershell.exe"),
            p(60, 1, "msedgewebview2.exe"),
        ];
        assert_eq!(
            selected_processes(&processes, 10),
            HashSet::from([10, 20, 30, 40, 50])
        );
    }

    #[test]
    fn sampler_reports_real_commit_and_releases_query_handles() {
        // WTS initialises Windows RPC/thread-pool handles on first use. Check
        // steady-state growth after native one-time setup, rather than treating
        // that initialisation as handles retained by each sample.
        for _ in 0..20 { let _ = memory_snapshot(); }
        let before = process_memory(std::process::id())["handles"]
            .as_u64()
            .unwrap();
        for _ in 0..100 {
            let sample = memory_snapshot();
            assert!(sample["system"]["commit_limit_bytes"].as_u64().unwrap() > 0);
            let own = sample["processes"]
                .as_array()
                .unwrap()
                .iter()
                .find(|p| p["pid"] == std::process::id())
                .unwrap();
            assert!(own["private_bytes"].as_u64().unwrap() > 0);
        }
        let after = process_memory(std::process::id())["handles"]
            .as_u64()
            .unwrap();
        assert!(
            after <= before + 4,
            "query handles leaked: {before} -> {after}"
        );
    }

    #[test]
    fn diagnostics_rotate_with_only_one_backup() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("diagnostics.jsonl");
        for i in 0..100 {
            append(&path, &json!({"sample":i}), 64).unwrap();
        }
        assert!(path.metadata().unwrap().len() <= 64);
        assert!(path.with_extension("jsonl.1").metadata().unwrap().len() <= 64);
        assert_eq!(std::fs::read_dir(dir.path()).unwrap().count(), 2);
        for line in std::fs::read_to_string(path).unwrap().lines() {
            assert!(serde_json::from_str::<Value>(line).is_ok());
        }
    }
}
