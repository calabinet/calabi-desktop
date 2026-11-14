use std::sync::{Arc, Mutex};
use tauri::{AppHandle, Manager, PhysicalPosition, PhysicalSize, WebviewWindow};
use crate::daemon::Supervisor;

#[derive(Default)]
pub struct LoginWindow(Mutex<WindowState>);

#[derive(Default)]
struct WindowState {
    compact: Option<bool>,
    workspace: Option<(PhysicalSize<u32>, PhysicalPosition<i32>, bool)>,
}

#[tauri::command]
pub fn desktop_set_login_window(window: WebviewWindow, app: AppHandle, compact: bool) -> Result<(), String> {
    let supervisor = app.state::<Option<Arc<Supervisor>>>();
    let sup = supervisor.as_ref().ok_or("Daemon unavailable")?;
    let expected = reqwest::Url::parse(&sup.url()).map_err(|e| e.to_string())?;
    if window.label() != "main" || window.url().map_err(|e| e.to_string())?.origin() != expected.origin() {
        return Err("Untrusted login UI origin".into());
    }
    let managed = app.state::<LoginWindow>();
    let mut state = managed.0.lock().map_err(|e| e.to_string())?;
    let changed = state.compact != Some(compact);
    if compact && changed {
        if state.compact == Some(false) {
            state.workspace = Some((window.inner_size().map_err(|e| e.to_string())?,
                window.outer_position().map_err(|e| e.to_string())?, window.is_maximized().map_err(|e| e.to_string())?));
        }
        if changed {
            window.set_fullscreen(false).map_err(|e| e.to_string())?;
            window.unmaximize().map_err(|e| e.to_string())?;
            window.set_min_size(Some(tauri::LogicalSize::new(360.0, 360.0))).map_err(|e| e.to_string())?;
        }
        let max_height = window.current_monitor().ok().flatten()
            .map(|m| m.work_area().size.height as f64 / m.scale_factor() - 48.0).unwrap_or(800.0).max(360.0);
        let height = 600.0_f64.min(max_height);
        window.set_size(tauri::LogicalSize::new(420.0, height)).map_err(|e| e.to_string())?;
        window.set_resizable(false).map_err(|e| e.to_string())?;
        window.set_maximizable(false).map_err(|e| e.to_string())?;
        if changed { window.center().map_err(|e| e.to_string())?; }
    } else if changed {
        window.set_resizable(true).map_err(|e| e.to_string())?;
        window.set_maximizable(true).map_err(|e| e.to_string())?;
        window.set_min_size(Some(tauri::LogicalSize::new(880.0, 560.0))).map_err(|e| e.to_string())?;
        if let Some((size, position, maximized)) = state.workspace {
            window.set_size(size).map_err(|e| e.to_string())?;
            window.set_position(position).map_err(|e| e.to_string())?;
            if maximized { window.maximize().map_err(|e| e.to_string())?; }
        } else {
            window.set_size(tauri::LogicalSize::new(1100.0, 720.0)).map_err(|e| e.to_string())?;
            window.center().map_err(|e| e.to_string())?;
        }
    }
    state.compact = Some(compact);
    Ok(())
}
