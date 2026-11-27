use std::sync::{Arc, Mutex};
use tauri::{AppHandle, Manager, PhysicalPosition, PhysicalSize, WebviewWindow};
use crate::daemon::Supervisor;

const BASE_LOGIN_WIDTH: f64 = 380.0;
const BASE_LOGIN_HEIGHT: f64 = 510.0;
const MIN_LOGIN_SIZE: f64 = 360.0;
const WORK_AREA_MARGIN: f64 = 48.0;
const MAX_CONTENT_SCALE: f64 = 2.5;
const SCALE_SNAP_TOLERANCE: f64 = 0.02;
const AXIS_MISMATCH_TOLERANCE: f64 = 0.15;

#[derive(Default)]
pub struct LoginWindow(Mutex<WindowState>);

#[derive(Default)]
struct WindowState {
    compact: Option<bool>,
    compact_size: Option<(u32, u32)>,
    workspace: Option<(PhysicalSize<u32>, PhysicalPosition<i32>, bool)>,
}

#[cfg(windows)]
fn os_text_scale_factor() -> f64 {
    use windows_sys::Win32::System::Registry::{RegGetValueW, HKEY_CURRENT_USER, RRF_RT_REG_DWORD};
    let subkey: Vec<u16> = "SOFTWARE\\Microsoft\\Accessibility\0".encode_utf16().collect();
    let value: Vec<u16> = "TextScaleFactor\0".encode_utf16().collect();
    let mut percent: u32 = 0;
    let mut size: u32 = std::mem::size_of::<u32>() as u32;
    let status = unsafe {
        RegGetValueW(
            HKEY_CURRENT_USER,
            subkey.as_ptr(),
            value.as_ptr(),
            RRF_RT_REG_DWORD,
            std::ptr::null_mut(),
            (&mut percent as *mut u32).cast(),
            &mut size,
        )
    };
    if status == 0 && (100..=250).contains(&percent) {
        percent as f64 / 100.0
    } else {
        1.0
    }
}

#[cfg(not(windows))]
fn os_text_scale_factor() -> f64 {
    1.0
}

fn resolve_content_scale(
    inner: PhysicalSize<u32>,
    monitor_scale: f64,
    viewport_width: Option<f64>,
    viewport_height: Option<f64>,
    os_text_scale: f64,
) -> f64 {
    let base = if os_text_scale.is_finite() {
        os_text_scale.clamp(1.0, MAX_CONTENT_SCALE)
    } else {
        1.0
    };
    if !monitor_scale.is_finite() || monitor_scale <= 0.0 || inner.width == 0 || inner.height == 0 {
        return base;
    }
    let sx = viewport_width
        .filter(|v| v.is_finite() && *v > 0.0)
        .map(|v| inner.width as f64 / (v * monitor_scale));
    let sy = viewport_height
        .filter(|v| v.is_finite() && *v > 0.0)
        .map(|v| inner.height as f64 / (v * monitor_scale));
    let measured = match (sx, sy) {
        (Some(x), Some(y)) => {
            let max_axis = x.max(y);
            if max_axis <= 0.0 || (x - y).abs() / max_axis > AXIS_MISMATCH_TOLERANCE {
                return base;
            }
            (x + y) * 0.5
        }
        (Some(x), None) => x,
        (None, Some(y)) => y,
        (None, None) => return base,
    };
    if !measured.is_finite() {
        return base;
    }
    if (measured - base).abs() < SCALE_SNAP_TOLERANCE {
        base
    } else {
        measured.clamp(1.0, MAX_CONTENT_SCALE)
    }
}

fn work_area_bounds(
    monitor: Option<&tauri::Monitor>,
    window_chrome_logical: Option<(f64, f64)>,
) -> Option<(f64, f64)> {
    let m = monitor?;
    let scale = m.scale_factor().max(1.0);
    let work = m.work_area().size;
    let (chrome_w, chrome_h) = window_chrome_logical.unwrap_or((WORK_AREA_MARGIN, WORK_AREA_MARGIN));
    Some((
        (work.width as f64 / scale - chrome_w.clamp(WORK_AREA_MARGIN, 120.0)).max(MIN_LOGIN_SIZE),
        (work.height as f64 / scale - chrome_h.clamp(WORK_AREA_MARGIN, 120.0)).max(MIN_LOGIN_SIZE),
    ))
}

fn compact_login_size(content_scale: f64, max_logical: Option<(f64, f64)>) -> (u32, u32) {
    let scale = if content_scale.is_finite() {
        content_scale.clamp(1.0, MAX_CONTENT_SCALE)
    } else {
        1.0
    };
    let (max_w, max_h) = max_logical.unwrap_or((1200.0, 800.0));
    let width = (BASE_LOGIN_WIDTH * scale).ceil().min(max_w.max(MIN_LOGIN_SIZE)).max(MIN_LOGIN_SIZE) as u32;
    let height = (BASE_LOGIN_HEIGHT * scale).ceil().min(max_h.max(MIN_LOGIN_SIZE)).max(MIN_LOGIN_SIZE) as u32;
    (width, height)
}

fn clamp_position_to_work_area(
    position: PhysicalPosition<i32>,
    outer_size: PhysicalSize<u32>,
    work_pos: PhysicalPosition<i32>,
    work_size: PhysicalSize<u32>,
) -> PhysicalPosition<i32> {
    let max_x = work_pos.x + work_size.width.saturating_sub(outer_size.width) as i32;
    let max_y = work_pos.y + work_size.height.saturating_sub(outer_size.height) as i32;
    PhysicalPosition::new(
        position.x.min(max_x).max(work_pos.x),
        position.y.min(max_y).max(work_pos.y),
    )
}

fn window_chrome_logical(
    inner: PhysicalSize<u32>,
    outer: Result<PhysicalSize<u32>, tauri::Error>,
    scale: f64,
) -> Option<(f64, f64)> {
    let outer = outer.ok()?;
    if !scale.is_finite() || scale <= 0.0 {
        return None;
    }
    let dw = outer.width.saturating_sub(inner.width) as f64 / scale;
    let dh = outer.height.saturating_sub(inner.height) as f64 / scale;
    Some((dw + 16.0, dh + 16.0))
}

pub fn refresh_compact_window(window: &tauri::Window) {
    let managed = window.app_handle().state::<LoginWindow>();
    let Ok(mut state) = managed.0.lock() else { return; };
    if state.compact != Some(true) {
        return;
    }
    let Ok(current_inner) = window.inner_size() else { return; };
    let Ok(current_scale) = window.scale_factor() else { return; };
    let monitor = window.current_monitor().ok().flatten();
    let chrome = window_chrome_logical(current_inner, window.outer_size(), current_scale);
    let max_logical = work_area_bounds(monitor.as_ref(), chrome);
    let target = compact_login_size(os_text_scale_factor(), max_logical);
    let size_changed = state.compact_size
        .map(|(w, h)| w.abs_diff(target.0) >= 2 || h.abs_diff(target.1) >= 2)
        .unwrap_or(true);
    if !size_changed {
        return;
    }
    if window.set_size(tauri::LogicalSize::new(target.0 as f64, target.1 as f64)).is_ok() {
        state.compact_size = Some(target);
        if let (Some(m), Ok(pos), Ok(outer)) = (monitor, window.outer_position(), window.outer_size()) {
            let clamped = clamp_position_to_work_area(pos, outer, m.work_area().position, m.work_area().size);
            if clamped != pos {
                let _ = window.set_position(clamped);
            }
        }
    }
}

#[tauri::command]
pub fn desktop_set_login_window(
    window: WebviewWindow,
    app: AppHandle,
    compact: bool,
    viewport_width: Option<f64>,
    viewport_height: Option<f64>,
) -> Result<(), String> {
    let supervisor = app.state::<Option<Arc<Supervisor>>>();
    let sup = supervisor.as_ref().ok_or("Daemon unavailable")?;
    let expected = reqwest::Url::parse(&sup.url()).map_err(|e| e.to_string())?;
    if window.label() != "main" || window.url().map_err(|e| e.to_string())?.origin() != expected.origin() {
        return Err("Untrusted login UI origin".into());
    }
    let managed = app.state::<LoginWindow>();
    let mut state = managed.0.lock().map_err(|e| e.to_string())?;
    let changed = state.compact != Some(compact);
    if compact {
        // Sample inner size and DPI before leaving fullscreen/maximized state so
        // they still correspond to the viewport dimensions reported by the WebView.
        let current_inner = window.inner_size().map_err(|e| e.to_string())?;
        let current_scale = window.scale_factor().map_err(|e| e.to_string())?;
        if changed {
            if state.compact == Some(false) {
                state.workspace = Some((
                    current_inner,
                    window.outer_position().map_err(|e| e.to_string())?,
                    window.is_maximized().map_err(|e| e.to_string())?,
                ));
            }
            window.set_fullscreen(false).map_err(|e| e.to_string())?;
            window.unmaximize().map_err(|e| e.to_string())?;
            window.set_min_size(Some(tauri::LogicalSize::new(MIN_LOGIN_SIZE, MIN_LOGIN_SIZE))).map_err(|e| e.to_string())?;
        }
        let monitor = window.current_monitor().ok().flatten();
        let chrome = window.inner_size().ok()
            .zip(window.scale_factor().ok())
            .and_then(|(inner, scale)| window_chrome_logical(inner, window.outer_size(), scale));
        let max_logical = work_area_bounds(monitor.as_ref(), chrome);
        let content_scale = resolve_content_scale(
            current_inner,
            current_scale,
            viewport_width,
            viewport_height,
            os_text_scale_factor(),
        );
        let target = compact_login_size(content_scale, max_logical);
        let size_changed = state.compact_size
            .map(|(w, h)| w.abs_diff(target.0) >= 2 || h.abs_diff(target.1) >= 2)
            .unwrap_or(true);
        if changed || size_changed {
            window.set_size(tauri::LogicalSize::new(target.0 as f64, target.1 as f64)).map_err(|e| e.to_string())?;
            state.compact_size = Some(target);
        }
        if changed {
            window.set_resizable(false).map_err(|e| e.to_string())?;
            window.set_maximizable(false).map_err(|e| e.to_string())?;
            window.center().map_err(|e| e.to_string())?;
        } else if size_changed {
            if let (Some(m), Ok(pos), Ok(outer)) = (monitor, window.outer_position(), window.outer_size()) {
                let clamped = clamp_position_to_work_area(pos, outer, m.work_area().position, m.work_area().size);
                if clamped != pos {
                    window.set_position(clamped).map_err(|e| e.to_string())?;
                }
            }
        }
    } else if changed {
        state.compact_size = None;
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

#[cfg(test)]
mod tests {
    use super::*;

    fn work_max(w: f64, h: f64) -> Option<(f64, f64)> {
        Some(((w - WORK_AREA_MARGIN).max(MIN_LOGIN_SIZE), (h - WORK_AREA_MARGIN).max(MIN_LOGIN_SIZE)))
    }

    #[test]
    fn normal_text_scale_keeps_exact_base_size() {
        let scale = resolve_content_scale(PhysicalSize::new(475, 638), 1.25, Some(380.0), Some(510.0), 1.0);
        assert_eq!(scale, 1.0);
        assert_eq!(compact_login_size(scale, work_max(1536.0, 825.0)), (380, 510));
    }

    #[test]
    fn enlarged_windows_text_scale_expands_login_window() {
        // 150% Windows text scaling on 100% DPI: 420x600 initial window yields 280x400 viewport.
        let scale = resolve_content_scale(PhysicalSize::new(420, 600), 1.0, Some(280.0), Some(400.0), 1.5);
        assert_eq!(scale, 1.5);
        assert_eq!(compact_login_size(scale, work_max(1920.0, 1032.0)), (570, 765));

        // 125% Windows text scaling on 125% DPI: 525x750 physical yields 336x480 viewport.
        let scale_125 = resolve_content_scale(PhysicalSize::new(525, 750), 1.25, Some(336.0), Some(480.0), 1.25);
        assert_eq!(scale_125, 1.25);
        assert_eq!(compact_login_size(scale_125, work_max(1536.0, 825.0)), (475, 638));
    }

    #[test]
    fn missing_viewport_falls_back_to_os_text_scale() {
        let scale = resolve_content_scale(PhysicalSize::new(420, 600), 1.0, None, None, 1.5);
        assert_eq!(scale, 1.5);
        assert_eq!(compact_login_size(scale, work_max(1920.0, 1032.0)), (570, 765));
    }

    #[test]
    fn mismatched_axes_from_transition_fall_back_to_os_text_scale() {
        let scale = resolve_content_scale(PhysicalSize::new(1100, 720), 1.0, Some(380.0), Some(510.0), 1.25);
        assert_eq!(scale, 1.25);
    }

    #[test]
    fn webview_zoom_beyond_os_text_scale_is_respected() {
        let scale = resolve_content_scale(PhysicalSize::new(420, 600), 1.0, Some(336.0), Some(480.0), 1.0);
        assert!((scale - 1.25).abs() < 1e-6);
        assert_eq!(compact_login_size(scale, work_max(1920.0, 1032.0)), (475, 638));
    }

    #[test]
    fn oversized_scale_clamps_to_monitor_work_area() {
        assert_eq!(compact_login_size(2.25, work_max(1280.0, 688.0)), (855, 640));
        assert_eq!(compact_login_size(2.25, work_max(700.0, 600.0)), (652, 552));
    }

    #[test]
    fn clamp_position_keeps_enlarged_window_inside_work_area() {
        let clamped = clamp_position_to_work_area(
            PhysicalPosition::new(1400, 600),
            PhysicalSize::new(600, 800),
            PhysicalPosition::new(0, 0),
            PhysicalSize::new(1920, 1032),
        );
        assert_eq!(clamped, PhysicalPosition::new(1320, 232));
    }

    #[test]
    fn os_text_scale_factor_stays_within_expected_bounds() {
        let scale = os_text_scale_factor();
        assert!((1.0..=MAX_CONTENT_SCALE).contains(&scale), "unexpected scale: {scale}");
    }
}
