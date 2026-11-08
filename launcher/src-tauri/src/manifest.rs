// SPDX-License-Identifier: GPL-3.0-or-later
//! The game manifest: `game.toml` next to the launcher exe (the release build copies recomp/games/<id>/game.toml there). It is the
//! one place that says which game this launcher belongs to: names of the exe, ini and log, the supported dump, the launcher's own
//! strings. Nothing else in the launcher names a game. For test runs `RECOMP_GAME_MANIFEST` names another file.

use serde::{Deserialize, Serialize};
use std::path::PathBuf;
use std::sync::OnceLock;

#[derive(Deserialize, Clone)]
pub struct Game {
    pub id: String,
    pub title: String,
    pub short_title: String,
    /// Program id (16 hex digits) of the game
    #[serde(default)]
    pub title_id: String,
}

#[derive(Deserialize, Clone)]
pub struct Files {
    pub exe: String,
    pub runtime_dll: String,
    pub ini: String,
    pub log: String,
}

#[derive(Deserialize, Clone)]
pub struct DumpFile {
    pub name: String,
}

#[derive(Deserialize, Clone)]
pub struct Dump {
    #[serde(default)]
    pub display_version: String,
    #[serde(default)]
    pub example_folder: String,
    #[serde(default)]
    pub file: Vec<DumpFile>,
}

#[derive(Deserialize, Clone)]
pub struct Module {
    pub name: String,
    pub file: String,
}

#[derive(Deserialize, Clone, Default)]
pub struct Shaders {
    #[serde(default)]
    pub extractor: String,
}

#[derive(Deserialize, Clone)]
pub struct Launcher {
    pub name: String,
    pub window_title: String,
    pub identifier: String,
    #[serde(default)]
    pub disclaimer: String,
}

#[derive(Deserialize, Clone)]
pub struct Manifest {
    pub game: Game,
    pub files: Files,
    pub dump: Dump,
    pub module: Vec<Module>,
    #[serde(default)]
    pub shaders: Shaders,
    pub launcher: Launcher,
}

/// What the page needs (the frontend never reads the file itself).
#[derive(Serialize, Clone)]
pub struct GameInfo {
    pub id: String,
    pub title: String,
    pub short_title: String,
    pub exe: String,
    pub ini: String,
    pub log: String,
    pub launcher_name: String,
    pub window_title: String,
    pub disclaimer: String,
    pub dump_version: String,
    pub dump_example: String,
    pub exefs_files: Vec<String>,
    pub shader_extractor: String,
}

impl Manifest {
    /// The exefs files the dump check wants, in the manifest's order (main, sdk, subsdk0, rtld, main.npdm).
    pub fn exefs_files(&self) -> Vec<String> {
        self.dump.file.iter().map(|f| f.name.clone()).collect()
    }

    /// The NSO of the module named "main": the shader extractor reads the engine's data out of it.
    pub fn main_file(&self) -> String {
        self.module.iter().find(|m| m.name == "main").map(|m| m.file.clone()).unwrap_or_else(|| "main".into())
    }

    /// The import library of the runtime DLL in the package (tools/build/sdk/<stem>.lib).
    pub fn runtime_lib(&self) -> String {
        let dll = &self.files.runtime_dll;
        format!("{}.lib", dll.rsplit_once('.').map(|(s, _)| s).unwrap_or(dll))
    }

    /// `<exe stem>_crash.txt` etc.: the reports the runtime writes next to the exe (diag/crash.cpp, watchdog.cpp), then the log.
    pub fn report_files(&self) -> Vec<String> {
        let exe = &self.files.exe;
        let stem = exe.rsplit_once('.').map(|(s, _)| s).unwrap_or(exe);
        vec![format!("{stem}_crash.txt"), format!("{stem}_freeze.txt"), format!("{stem}_fatal.txt"), self.files.log.clone()]
    }

    /// The prefix of the launcher's test-run variable: `<ID>_LAUNCHER_EVAL_FILE`.
    pub fn eval_env_name(&self) -> String {
        format!("{}_LAUNCHER_EVAL_FILE", self.game.id.to_uppercase())
    }

    pub fn info(&self) -> GameInfo {
        GameInfo {
            id: self.game.id.clone(),
            title: self.game.title.clone(),
            short_title: self.game.short_title.clone(),
            exe: self.files.exe.clone(),
            ini: self.files.ini.clone(),
            log: self.files.log.clone(),
            launcher_name: self.launcher.name.clone(),
            window_title: self.launcher.window_title.clone(),
            disclaimer: self.launcher.disclaimer.clone(),
            dump_version: self.dump.display_version.clone(),
            dump_example: self.dump.example_folder.clone(),
            exefs_files: self.exefs_files(),
            shader_extractor: self.shaders.extractor.clone(),
        }
    }
}

fn manifest_path() -> PathBuf {
    if let Ok(p) = std::env::var("RECOMP_GAME_MANIFEST") {
        return PathBuf::from(p);
    }
    let dir = std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.to_path_buf())).unwrap_or_else(|| PathBuf::from("."));
    dir.join("game.toml")
}

fn load() -> Result<Manifest, String> {
    let path = manifest_path();
    let text = std::fs::read_to_string(&path).map_err(|e| {
        format!("The game description {} could not be read ({}). Extract the release zip again so game.toml sits next to the launcher.", path.display(), e)
    })?;
    toml::from_str(&text).map_err(|e| format!("The game description {} is not valid: {}", path.display(), e))
}

/// The manifest, read once. The error says what is wrong in the player's words.
pub fn get() -> Result<&'static Manifest, String> {
    static M: OnceLock<Result<Manifest, String>> = OnceLock::new();
    M.get_or_init(load).as_ref().map_err(|e| e.clone())
}
