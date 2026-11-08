"""Portable Eden setup for the reference harness (work area S5).

  python eden_config.py setup [--eden-dir work/eden] [--nsp PATH]
      Creates <eden-dir>/user/{config,log,screenshots}, copies eden.exe/eden-cli.exe from toolchain/eden when missing (portable mode = a `user`
      folder next to the exe) and writes the config settings the harness needs into user/config/sdl2-config.ini (created by a first short run of eden-cli when missing).
  python eden_config.py set KEY=VALUE ...      change SDL config keys, e.g. Debugging.use_gdbstub=true  Renderer.resolution_setup=2

The config is Qt-style ini: `key=value` plus a `key\\default=true|false` line saying whether the value is the built-in default. A value written by this
module also sets its \\default line to false.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# What the harness wants, section.key = value.
HARNESS_SETTINGS = {
    "Debugging.use_gdbstub": "false",          # eden_run.py turns it on for runs that sample state: with the stub on Eden WAITS for a GDB client at start
    "Debugging.gdbstub_port": "6543",
    "Core.use_speed_limit": "true",
    "Renderer.use_asynchronous_shaders": "false",
    "Renderer.resolution_setup": "3",           # 1x (native 1280x720 docked is 1920x1080 at 1.5x: the game renders 720p/1080p itself)
    "Renderer.use_vsync": "2",
    "Audio.output_engine": "3",                 # null sink: no audio device in an unattended session
    "UI.confirmStop": "0",
}


def sh(*a, **k):
    return subprocess.run(a, **k)


def read_ini(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def set_values(lines, settings):
    """Returns new lines with `Section.key` values replaced (key and its \\default line) or appended to the section."""
    by_sec = {}
    for sk, v in settings.items():
        sec, key = sk.split(".", 1)
        by_sec.setdefault(sec, {})[key] = v
    out, sec = [], None
    seen = set()
    default_seen = set()

    def flush_section(name):
        for key, v in by_sec.get(name, {}).items():
            if (name, key) not in seen:
                if (name, key) not in default_seen:
                    out.append(f"{key}\\default=false")
                out.append(f"{key}={v}")
                seen.add((name, key))

    for l in lines:
        m = re.match(r"\[(.*)\]\s*$", l)
        if m:
            if sec is not None:
                flush_section(sec)
            sec = m.group(1)
            out.append(l)
            continue
        if sec in by_sec:
            m2 = re.match(r"([^=\\]+)\\default=", l)
            if m2 and m2.group(1) in by_sec[sec]:
                out.append(f"{m2.group(1)}\\default=false")
                default_seen.add((sec, m2.group(1)))
                continue
            m3 = re.match(r"([^=\\]+)=", l)
            if m3 and m3.group(1) in by_sec[sec]:
                out.append(f"{m3.group(1)}={by_sec[sec][m3.group(1)]}")
                seen.add((sec, m3.group(1)))
                continue
        out.append(l)
    if sec is not None:
        flush_section(sec)
    for name in by_sec:
        if not any((name, k) in seen for k in by_sec[name]):
            out.append(f"[{name}]")
            flush_section(name)
    return out


def setup(eden_dir, nsp):
    eden_dir = os.path.abspath(eden_dir)
    src = os.path.join(ROOT, "toolchain", "eden")
    for sub in ("config", "log", "screenshots", "nand", "sdmc", "dump"):
        os.makedirs(os.path.join(eden_dir, "user", sub), exist_ok=True)
    for exe in ("eden.exe", "eden-cli.exe"):
        if not os.path.exists(os.path.join(eden_dir, exe)):
            shutil.copy2(os.path.join(src, exe), os.path.join(eden_dir, exe))
    # This tool handles no key files: an emulator that needs them is set up by the user, outside this repository.
    cfg = os.path.join(eden_dir, "user", "config", "sdl2-config.ini")
    if not os.path.exists(cfg):
        # a first start writes the default config; give it a moment and stop it
        p = subprocess.Popen([os.path.join(eden_dir, "eden-cli.exe")] + ([nsp] if nsp else []), cwd=eden_dir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(60):
            time.sleep(0.5)
            if os.path.exists(cfg):
                break
        time.sleep(1.0)
        p.kill()
    apply(cfg, HARNESS_SETTINGS)
    print("eden dir ready:", eden_dir)


def apply(cfg, settings):
    lines = read_ini(cfg)
    new = set_values(lines, settings)
    with open(cfg, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(new) + "\n")


def main(argv=None):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("setup")
    s.add_argument("--eden-dir", default=os.path.join(ROOT, "work", "eden"))
    s.add_argument("--nsp")
    t = sub.add_parser("set")
    t.add_argument("--eden-dir", default=os.path.join(ROOT, "work", "eden"))
    t.add_argument("pairs", nargs="+")
    a = ap.parse_args(argv)
    if a.cmd == "setup":
        setup(a.eden_dir, a.nsp)
    else:
        kv = dict(p.split("=", 1) for p in a.pairs)
        apply(os.path.join(a.eden_dir, "user", "config", "sdl2-config.ini"), kv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
