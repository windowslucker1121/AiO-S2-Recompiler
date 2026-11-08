// SPDX-License-Identifier: GPL-3.0-or-later
//! ZBIC: an ExeFS whose NSOs use ZBIC compression (NSO flag bit 7, runtime/docs/zbic.md) is converted to plain NSOs in
//! `<game>/build/exefs_plain` before the game build, because the lifter reads plain NSOs only. The runtime reads ZBIC itself, so the
//! game's ini keeps pointing at the dump's own ExeFS. The work is done by `tools/extract/nx_plain_nso.exe` (tools/extract); it reads
//! the already-decrypted dump only (no keys, no containers).
//! No crate-local imports, so it can be built alone like gamebuild.rs.

use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

/// The converter in the package (`tools/extract/nx_plain_nso.exe`), or `RECOMP_NX_PLAIN_NSO` for development builds.
pub fn find_tool(tools_dir: &Path) -> Result<PathBuf, String> {
    if let Ok(p) = std::env::var("RECOMP_NX_PLAIN_NSO") {
        let p = PathBuf::from(p);
        if p.is_file() {
            return Ok(p);
        }
    }
    let p = tools_dir.join("extract").join("nx_plain_nso.exe");
    if p.is_file() {
        Ok(p)
    } else {
        Err(format!("The ZBIC converter {} is missing. Extract the release zip again so tools/ is complete.", p.display()))
    }
}

/// True when an NSO in the ExeFS folder uses ZBIC compression (NSO0 flags bit 7).
pub fn exefs_has_zbic(exefs: &Path) -> bool {
    let Ok(rd) = fs::read_dir(exefs) else { return false };
    rd.filter_map(|e| e.ok()).any(|e| {
        let mut h = [0u8; 16];
        fs::File::open(e.path()).and_then(|mut f| f.read_exact(&mut h)).is_ok() && &h[..4] == b"NSO0" && h[12] & 0x80 != 0
    })
}

/// The ExeFS the game build should lift: the dump's own, or, when it holds ZBIC modules, a plain copy in `<game>/build/exefs_plain`.
pub fn exefs_for_build(tool: Option<&Path>, exefs: &Path, game_dir: &Path) -> Result<PathBuf, String> {
    if !exefs_has_zbic(exefs) {
        return Ok(exefs.to_path_buf());
    }
    let tool = tool.ok_or("The game program uses ZBIC compression and the converter (tools/extract/nx_plain_nso.exe) is missing.")?;
    let out = game_dir.join("build").join("exefs_plain");
    let mut cmd = Command::new(tool);
    cmd.arg(exefs).arg(&out);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        // CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS
        cmd.creation_flags(0x0800_0000 | 0x0000_4000);
    }
    let res = cmd.stdout(Stdio::null()).stderr(Stdio::piped()).output().map_err(|e| format!("could not start the ZBIC converter: {}", e))?;
    if !res.status.success() {
        let text = String::from_utf8_lossy(&res.stderr);
        let error = text.lines().find_map(|l| l.strip_prefix("@error ")).map(|s| s.to_string());
        return Err(error.unwrap_or_else(|| format!("Decompressing the ZBIC game program failed (exit {}).", res.status.code().unwrap_or(1))));
    }
    Ok(out)
}
