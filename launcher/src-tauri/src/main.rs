// SPDX-License-Identifier: GPL-3.0-or-later
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod dlss;
mod dump;
mod gamebuild;
mod ini;
mod manifest;
mod play;
mod settings;
mod shaders;
mod zbic;

use serde::{Deserialize, Serialize};
use std::fs;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use tauri::Emitter;
use tauri::{AppHandle, Manager, State};
use tauri_plugin_dialog::DialogExt;

/// The launcher's own options, stored in the user's app config folder.
#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
struct Config {
    /// Root of the decrypted dump (the folder with base/ and updates/).
    dump_dir: String,
    /// Folder with the game exe. Empty: the folder the launcher is in.
    game_dir: String,
    minimize_on_play: bool,
}

fn config_path(app: &AppHandle) -> Result<PathBuf, String> {
    // %APPDATA%\<launcher.identifier>: one settings folder per game, whatever app id the build was given
    let id = manifest::get()?.launcher.identifier.clone();
    let dir = app.path().config_dir().map_err(|e| e.to_string())?.join(id);
    fs::create_dir_all(&dir).map_err(|e| format!("cannot create {}: {}", dir.display(), e))?;
    Ok(dir.join("launcher.json"))
}

fn load_config(app: &AppHandle) -> Config {
    config_path(app)
        .ok()
        .and_then(|p| fs::read_to_string(p).ok())
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default()
}

fn exe_dir() -> PathBuf {
    std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.to_path_buf())).unwrap_or_else(|| PathBuf::from("."))
}

fn game_dir_of(cfg: &Config) -> PathBuf {
    if cfg.game_dir.trim().is_empty() {
        exe_dir()
    } else {
        PathBuf::from(cfg.game_dir.trim())
    }
}

#[derive(Serialize)]
struct Overview {
    game_dir: String,
    default_game_dir: String,
    exe_exists: bool,
}

/// The game manifest as the pages need it.
#[tauri::command]
fn get_game() -> Result<manifest::GameInfo, String> {
    Ok(manifest::get()?.info())
}

#[tauri::command]
fn get_config(app: AppHandle) -> Config {
    load_config(&app)
}

#[tauri::command]
fn save_config(app: AppHandle, config: Config) -> Result<(), String> {
    let text = serde_json::to_string_pretty(&config).map_err(|e| e.to_string())?;
    fs::write(config_path(&app)?, text).map_err(|e| format!("cannot save launcher settings: {}", e))
}

#[tauri::command]
fn get_overview(app: AppHandle) -> Overview {
    let cfg = load_config(&app);
    let dir = game_dir_of(&cfg);
    Overview {
        game_dir: dump::fwd(&dir),
        default_game_dir: dump::fwd(&exe_dir()),
        exe_exists: manifest::get().map_or(false, |m| dir.join(&m.files.exe).is_file()),
    }
}

#[tauri::command]
async fn pick_folder(app: AppHandle, title: String, start: Option<String>) -> Option<String> {
    let mut dlg = app.dialog().file().set_title(title);
    if let Some(s) = start.filter(|s| PathBuf::from(s).is_dir()) {
        dlg = dlg.set_directory(s);
    }
    dlg.blocking_pick_folder().and_then(|p| p.into_path().ok()).map(|p| dump::fwd(&p))
}

#[tauri::command]
fn validate_dump(path: String) -> dump::DumpReport {
    dump::scan(&path)
}

/// Validates, stores the dump folder, and points `[fs] romfs` in the game ini at the chosen romfs.
#[tauri::command]
fn save_dump(app: AppHandle, path: String) -> Result<dump::DumpReport, String> {
    let report = dump::scan(&path);
    let chosen = report.chosen.clone().ok_or_else(|| report.errors.join(" "))?;
    let m = manifest::get()?;
    let mut cfg = load_config(&app);
    cfg.dump_dir = report.path.clone();
    save_config(app.clone(), cfg.clone())?;
    settings::apply(
        &game_dir_of(&cfg),
        &m.files.ini,
        &[
            settings::Edit { file: "game".into(), section: "fs".into(), key: "exefs".into(), value: chosen.exefs },
            settings::Edit { file: "game".into(), section: "fs".into(), key: "romfs".into(), value: chosen.romfs },
        ],
    )?;
    Ok(report)
}

#[tauri::command]
fn read_settings(app: AppHandle) -> Result<settings::SettingsDump, String> {
    Ok(settings::read(&game_dir_of(&load_config(&app)), &manifest::get()?.files.ini))
}

#[tauri::command]
fn write_settings(app: AppHandle, edits: Vec<settings::Edit>) -> Result<(), String> {
    settings::apply(&game_dir_of(&load_config(&app)), &manifest::get()?.files.ini, &edits)
}

#[tauri::command]
fn play(app: AppHandle) -> Result<(), String> {
    let cfg = load_config(&app);
    if cfg.dump_dir.trim().is_empty() {
        return Err("Choose your game files first (Setup).".into());
    }
    let report = dump::scan(&cfg.dump_dir);
    let chosen = report.chosen.ok_or_else(|| format!("The game files at {} are not usable: {}", cfg.dump_dir, report.errors.join(" ")))?;
    let m = manifest::get()?;
    let dir = game_dir_of(&cfg);
    if gamebuild::is_package_b(&exe_dir().join("tools")) && !gamebuild::is_built(&dir, &m.files.exe) {
        return Err("Build the game first (Game files, Set up).".into());
    }
    // the shader cache may be incomplete: the runtime translates missing shaders while the game runs
    settings::ensure_play_paths(&dir, &m.files.ini, &chosen.exefs, &chosen.romfs)?;
    play::start(&app, &dir, cfg.minimize_on_play)
}

/// Shader setup: the running job's progress and its cancel flag.
#[derive(Default)]
struct SetupState {
    progress: Mutex<shaders::Progress>,
    cancel: AtomicBool,
}

#[derive(Serialize)]
struct SetupStatus {
    progress: shaders::Progress,
    /// setup.json says finished and the cache is there
    complete: bool,
    /// an earlier setup started (Set up continues it)
    started: bool,
    /// the game exe is built: Play works, the shader cache may still be filling
    playable: bool,
    shaders_done: u64,
    shaders_total: u64,
}

#[tauri::command]
fn setup_status(app: AppHandle, state: State<Arc<SetupState>>) -> Result<SetupStatus, String> {
    let m = manifest::get()?;
    let dir = game_dir_of(&load_config(&app));
    let mut progress = state.progress.lock().unwrap().clone();
    if progress.state.is_empty() {
        progress.state = "idle".into();
    }
    let built = !gamebuild::is_package_b(&exe_dir().join("tools")) || gamebuild::is_built(&dir, &m.files.exe);
    let started = !shaders::read_marker(&dir).romfs.is_empty() || dir.join("build").join("state.json").is_file();
    let (shaders_done, shaders_total) = shaders::cache_progress(&dir);
    Ok(SetupStatus { progress, complete: built && shaders::is_complete(&dir), started, playable: built && dir.join(&m.files.exe).is_file(), shaders_done, shaders_total })
}

/// First-run setup: (a) the game ini paths,
/// (b) in a model (b) release, the game exe built from the player's dump (gamebuild.rs; ZBIC modules converted to plain NSOs first),
/// (c) the shader cache in shader_cache/spirv, on a worker thread with "setup-status" events.
#[tauri::command]
fn start_setup(app: AppHandle, state: State<Arc<SetupState>>) -> Result<(), String> {
    let m = manifest::get()?;
    let cfg = load_config(&app);
    dump::scan(&cfg.dump_dir).chosen.ok_or_else(|| "Choose usable game files first.".to_string())?;
    let dir = game_dir_of(&cfg);
    let tools_dir = exe_dir().join("tools");
    let package_b = gamebuild::is_package_b(&tools_dir);
    if !package_b && !dir.join(&m.files.exe).is_file() {
        return Err(format!("{0} was not found in {1}. Put the launcher next to {0} or set the game folder in Settings.", m.files.exe, dump::fwd(&dir)));
    }
    let zbic_tool = zbic::find_tool(&tools_dir).ok();
    {
        let mut p = state.progress.lock().unwrap();
        if p.state == "running" {
            return Err("Setup is already running.".into());
        }
        *p = shaders::Progress { state: "running".into(), phase: "check".into(), message: "Checking the setup tools...".into(), ..Default::default() };
    }
    let st = state.inner().clone();
    st.cancel.store(false, Ordering::SeqCst);
    let jobs = std::thread::available_parallelism().map(|n| n.get().saturating_sub(1)).unwrap_or(2).max(1);
    std::thread::spawn(move || {
        let emit = |p: shaders::Progress| {
            *st.progress.lock().unwrap() = p.clone();
            let _ = app.emit("setup-status", p);
        };
        let result = (|| -> Result<bool, String> {
            let report = dump::scan(&cfg.dump_dir);
            let chosen = report.chosen.ok_or_else(|| format!("The game files at {} are not usable: {}", cfg.dump_dir, report.errors.join(" ")))?;
            settings::ensure_play_paths(&dir, &m.files.ini, &chosen.exefs, &chosen.romfs)?;
            // the lifter and the shader extractor read plain NSOs; the runtime reads ZBIC itself, so the ini keeps the dump's ExeFS
            let exefs = zbic::exefs_for_build(zbic_tool.as_deref(), &PathBuf::from(&chosen.exefs), &dir)?;
            if package_b && !gamebuild::is_built(&dir, &m.files.exe) {
                let tools = gamebuild::find_tools(&tools_dir, &m.runtime_lib())?;
                let build_job = gamebuild::Job { game_dir: dir.clone(), exefs: exefs.clone(), jobs: jobs + 1 };
                let built = gamebuild::run(&build_job, &tools, &st.cancel, |t| {
                    emit(shaders::Progress { state: "running".into(), phase: t.phase, done: t.done, total: t.total, message: t.message })
                })?;
                if !built {
                    return Ok(false);
                }
            }
            let tools = shaders::find_tools(&tools_dir)?;
            let job = shaders::Job { game_dir: dir.clone(), romfs: PathBuf::from(&chosen.romfs), main: exefs.join(m.main_file()), jobs: 0, limit: 0 };
            shaders::run(&job, &tools, &st.cancel, |p| emit(p))
        })();
        let end = match result {
            Ok(true) => shaders::Progress { state: "done".into(), phase: "finish".into(), message: "Setup finished.".into(), ..Default::default() },
            Ok(false) => shaders::Progress { state: "cancelled".into(), message: "Setup paused. Set up again continues where it stopped.".into(), ..Default::default() },
            Err(e) => shaders::Progress { state: "error".into(), message: e, ..Default::default() },
        };
        emit(end);
    });
    Ok(())
}

#[tauri::command]
fn cancel_setup(state: State<Arc<SetupState>>) {
    state.cancel.store(true, Ordering::SeqCst);
}

#[tauri::command]
fn dlss_status(app: AppHandle) -> dlss::Status {
    dlss::status(&game_dir_of(&load_config(&app)), &exe_dir().join("tools"))
}

/// Downloads NVIDIA's nvngx_dlss.dll (see dlss.rs) on a worker thread, with "dlss-progress" events.
#[tauri::command]
async fn fetch_dlss(app: AppHandle) -> Result<(), String> {
    let dir = game_dir_of(&load_config(&app));
    let tools_dir = exe_dir().join("tools");
    tauri::async_runtime::spawn_blocking(move || {
        dlss::fetch(&dir, &tools_dir, |p| {
            let _ = app.emit("dlss-progress", p);
        })
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
fn game_status(state: State<play::PlayState>) -> play::Status {
    state.0.lock().unwrap().clone()
}

#[tauri::command]
fn open_game_file(app: AppHandle, name: String) -> Result<(), String> {
    if !manifest::get()?.report_files().contains(&name) {
        return Err("That file cannot be opened from here.".into());
    }
    let path = game_dir_of(&load_config(&app)).join(&name);
    if !path.is_file() {
        return Err(format!("{} does not exist.", name));
    }
    play::open_with_shell(&path)
}

#[tauri::command]
fn open_game_dir(app: AppHandle) -> Result<(), String> {
    play::open_with_shell(&game_dir_of(&load_config(&app)))
}


/// Test runs only (<ID>_LAUNCHER_EVAL_FILE set, the id of the manifest uppercased): the page writes a result next to the eval file, <file>.out.
#[tauri::command]
fn test_report(text: String) -> Result<(), String> {
    let path = std::env::var(manifest::get()?.eval_env_name()).map_err(|_| "not a test run".to_string())?;
    std::fs::write(format!("{path}.out"), text).map_err(|e| e.to_string())
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(play::PlayState::default())
        .manage(Arc::new(SetupState::default()))
        .setup(|app| {
            // The window carries the game's launcher title from the manifest (tauri.conf.json has a neutral one).
            if let (Ok(m), Some(win)) = (manifest::get(), app.get_webview_window("main")) {
                let _ = win.set_title(&m.launcher.window_title);
            }
            // Test runs drive the page without input: <ID>_LAUNCHER_EVAL_FILE=<path>. A JS file written there is run in the page once and
            // deleted; it reports back through document.title (the window title). WebView2's DevTools port does not open for Tauri apps here.
            if let Some(path) = manifest::get().ok().and_then(|m| std::env::var(m.eval_env_name()).ok()) {
                if let Some(win) = app.get_webview_window("main") {
                    std::thread::spawn(move || loop {
                        std::thread::sleep(std::time::Duration::from_millis(300));
                        if let Ok(js) = std::fs::read_to_string(&path) {
                            let _ = std::fs::remove_file(&path);
                            let _ = win.eval(&js);
                        }
                    });
                }
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            test_report,
            get_game,
            get_config,
            save_config,
            get_overview,
            pick_folder,
            validate_dump,
            save_dump,
            read_settings,
            write_settings,
            play,
            setup_status,
            start_setup,
            cancel_setup,
            dlss_status,
            fetch_dlss,
            game_status,
            open_game_file,
            open_game_dir,
        ])
        .run(tauri::generate_context!())
        .expect("error while running the launcher");
}
