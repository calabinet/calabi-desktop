use std::sync::Arc;
use tauri::{AppHandle, Manager, WebviewWindow};
use crate::daemon::Supervisor;

#[tauri::command]
pub fn desktop_set_theme(window: WebviewWindow, app: AppHandle, mode: String) -> Result<(), String> {
    let supervisor = app.state::<Option<Arc<Supervisor>>>();
    let sup = supervisor.as_ref().ok_or("Daemon unavailable")?;
    let expected = reqwest::Url::parse(&sup.url()).map_err(|e| e.to_string())?;
    if window.label() != "main" || window.url().map_err(|e| e.to_string())?.origin() != expected.origin() {
        return Err("Untrusted appearance UI origin".into());
    }
    let theme = match mode.as_str() {
        "system" => None,
        "light" => Some(tauri::Theme::Light),
        "dark" => Some(tauri::Theme::Dark),
        _ => return Err("Invalid appearance mode".into()),
    };
    window.set_theme(theme).map_err(|e| e.to_string())
}
