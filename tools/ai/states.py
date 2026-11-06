"""Saved states and bug bundles for gamectl.Game: the tools states, save_state, load_state, capture_bug, bugs (a mixin of Game).

A state is not a process snapshot (a static recompiler's guest threads live on host stacks of lifted C++ frames, next to kernel, service
and Vulkan objects that cannot be written out and recreated). It is what the game itself can restore plus the way back to the moment:
  work/states/<game>/<name>/state.json   name, description, created, build (exe size/time/SHA-256, commit), settings, env, reach, ready
                           save/          the game's committed save data (copied from [fs] save_dir, never from or into the user's own)
                           shot.png       the window when the state was saved
load_state copies save/ into <run dir>/save, launches the exe with [fs] save_dir there, then runs the reach steps (boot to the title,
load the save, wait until in game) and the ready check. Built-in states (no save of their own, e.g. a fresh New Game) and the routes
come from recomp/games/<id>/ai_states.toml: the game knowledge stays in the game's folder.

Bug bundles (runtime nxrt/bugcapture.h) are folders under work/bugs: the F9 hotkey in the game writes one, capture_bug asks the running
game for one through a request file (<exe dir>/capture_bug.<pid>) and adds what only this side has (the window picture, the launch
arguments, stdout/stderr); with no game running it writes the bundle itself from the last run's files.
"""
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import time
import tomllib
from pathlib import Path

SKIP_SAVE = re.compile(r'\.(work|journal)\.\d+$|\.commit$|\.taken\.')     # journal work folders, journals, commit markers (fs_volume.h)
STEP_KINDS = ('route', 'wait_for', 'input', 'sleep', 'press_until', 'shot')
# Environment of a bug bundle (env.txt) that belongs to that run only, not to the moment: not carried into load_state(bug=...).
RUN_ONLY_ENV = re.compile(r'^NXRT_(TEST_|AI_|FS_SAVE_DIR$|BUG_DIR$|LOG$|INI$)')
_SHA = {}


def now_text():
    return time.strftime('%Y-%m-%d %H:%M:%S')


def file_sha256(p):
    st = p.stat()
    key = (str(p), st.st_size, st.st_mtime_ns)
    if key not in _SHA:
        h = hashlib.sha256()
        with open(p, 'rb') as f:
            for b in iter(lambda: f.read(1 << 22), b''):
                h.update(b)
        _SHA[key] = h.hexdigest()
    return _SHA[key]


def git(root, *args):
    try:
        r = subprocess.run(['git', '-C', str(root), *args], capture_output=True, text=True, timeout=20,
                           creationflags=subprocess.CREATE_NO_WINDOW)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return r.stdout.strip() if r.returncode == 0 else None


def copy_save(src, dst):
    """The committed state of a save folder into dst (replaced): no work folders, journals or commit markers. Returns the file count."""
    src, dst = Path(src), Path(dst)
    for _ in range(40):                                        # a commit being applied changes the committed folder: wait for it
        if not any(p.suffix == '.commit' for p in src.rglob('*.commit')):
            break
        time.sleep(0.05)
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    n = 0
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if not SKIP_SAVE.search(d)]
        rel = Path(root).relative_to(src)
        (dst / rel).mkdir(parents=True, exist_ok=True)
        for f in files:
            if not SKIP_SAVE.search(f):
                shutil.copy2(Path(root, f), dst / rel / f)
                n += 1
    return n


def count_files(p):
    return sum(1 for q in Path(p).rglob('*') if q.is_file()) if p and Path(p).exists() else 0


def read_json(p):
    try:
        return json.loads(Path(p).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None


def wincap_black(img):
    import wincap
    return wincap.is_black(img)


def jpeg(path, width=1280):
    from PIL import Image
    img = Image.open(path).convert('RGB')
    img.thumbnail((width, width))
    buf = io.BytesIO()
    img.save(buf, 'JPEG', quality=85)
    return buf.getvalue()


class StateTools:
    """Methods of gamectl.Game (it sets self.id, self.exe, self.run_dir, self.last_launch, launch, wait_for, send_input, ...)."""

    # ---- places -----------------------------------------------------------------------------------------------------------------------------
    def _root(self):
        from gamectl import ROOT
        return ROOT

    def _states_dir(self):
        return Path(os.environ.get('NXRT_AI_STATES_DIR') or self._root() / 'work' / 'states' / self.id).resolve()

    def _bugs_dir(self):
        """Where bundles go: NXRT_AI_BUGS_DIR, else work/bugs of the checkout (the development exe's [bug] dir default; launch passes it)."""
        return Path(os.environ.get('NXRT_AI_BUGS_DIR') or self._root() / 'work' / 'bugs').resolve()

    def _data_root(self):
        """The checkout that has the game data (game/<id>): this one, else the main checkout of this worktree."""
        root = self._root()
        if (root / 'game' / self.id).exists():
            return root
        common = git(root, 'rev-parse', '--path-format=absolute', '--git-common-dir')
        if common and (Path(common).parent / 'game' / self.id).exists():
            return Path(common).parent
        return root

    def _game_ai(self):
        """recomp/games/<id>/ai_states.toml with {data} / {root} placeholders filled in ({} when the game has none)."""
        p = Path(os.environ.get('NXRT_AI_STATES_TOML') or self._root() / 'recomp' / 'games' / self.id / 'ai_states.toml')
        if not p.exists():
            return {}
        subst = {'{data}': self._data_root().as_posix(), '{root}': self._root().as_posix()}

        def fill(v):
            if isinstance(v, str):
                for k, r in subst.items():
                    v = v.replace(k, r)
                return v
            if isinstance(v, list):
                return [fill(x) for x in v]
            if isinstance(v, dict):
                return {k: fill(x) for k, x in v.items()}
            return v
        return fill(tomllib.loads(p.read_text(encoding='utf-8')))

    def _build(self):
        """The exe on disk now: path, size, time, SHA-256, and the checkout's commit (HEAD now; the exe's own commit is in its log and bug.json)."""
        b = {'exe': str(self.exe)}
        if self.exe.exists():
            st = self.exe.stat()
            b.update(size=st.st_size, modified=time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(st.st_mtime)), sha256=file_sha256(self.exe))
        checkout = next((p for p in self.exe.parents if (p / '.git').exists()), self._root())
        b.update(checkout=str(checkout), checkout_commit=git(checkout, 'rev-parse', 'HEAD'),
                 checkout_dirty=bool(git(checkout, 'status', '--porcelain', '--untracked-files=no')))
        return b

    def _log_build(self):
        """The commit the running (or last) exe was built from, from its log line "boot: built from commit <sha>[ +changes]"."""
        lp = self._log_path()
        if lp.exists():
            with open(lp, encoding='utf-8', errors='replace') as f:
                for i, ln in enumerate(f):
                    if m := re.search(r'built from commit (\w*)( with uncommitted changes)?', ln):
                        return {'commit': m.group(1), 'dirty': bool(m.group(2))}
                    if i > 400:
                        break
        return {}

    def _last_launch(self):
        """The last launch's arguments: this object's, else <run dir>/launch.json (a CLI call is a new process)."""
        return self.last_launch or read_json(self.run_dir / 'launch.json') or {}

    def _save_dir_in_use(self):
        """The save folder of the running or last run: its log line "save data in '...'", else the launch settings, else <run dir>/save."""
        lp = self._log_path()
        if lp.exists():
            with open(lp, encoding='utf-8', errors='replace') as f:
                for i, ln in enumerate(f):
                    if m := re.search(r"save data in '([^']+)'", ln):
                        return Path(m.group(1))
                    if i > 400:
                        break
        s = self._last_launch().get('settings') or {}
        return Path(s['fs.save_dir']) if s.get('fs.save_dir') else self.run_dir / 'save'

    # ---- states -----------------------------------------------------------------------------------------------------------------------------
    def _state(self, name):
        """A saved state (work/states) or a built-in one (ai_states.toml [state.<name>]) as one dict."""
        d = self._states_dir() / name
        if (d / 'state.json').exists():
            s = read_json(d / 'state.json') or {}
            s.update(name=name, dir=str(d), kind='saved', save=str(d / 'save') if (d / 'save').exists() else None)
            return s
        b = (self._game_ai().get('state') or {}).get(name)
        if b is None:
            raise ValueError(f'no state {name!r}: states() lists them')
        return {**b, 'name': name, 'kind': 'built-in', 'dir': str(self._root() / 'recomp' / 'games' / self.id), 'save': None}

    def states(self):
        """The states load_state can start from: saved ones (work/states/<game>/<name>: the game's save data + how to get back there) and
        built-in ones of the game (recomp/games/<id>/ai_states.toml: no save of their own, e.g. a fresh New Game up to the first scene).
        Per state: description, reach (the route names run after the launch), ready check, save files, created, and whether the exe on disk
        is the build it was saved with (same SHA-256). Also the routes the game defines."""
        cur = self._build().get('sha256')
        out = []
        for name, b in (self._game_ai().get('state') or {}).items():
            out.append({'name': name, 'kind': 'built-in', 'description': b.get('description', ''), 'reach': b.get('reach', [])})
        sd = self._states_dir()
        for d in sorted(sd.iterdir()) if sd.exists() else []:
            s = read_json(d / 'state.json')
            if not s:
                continue
            out.append({'name': d.name, 'kind': 'saved', 'description': s.get('description', ''), 'created': s.get('created'),
                        'reach': s.get('reach', []), 'save_files': count_files(d / 'save'), 'from_bug': s.get('from_bug'),
                        'build_commit': (s.get('build') or {}).get('commit'), 'same_exe': (s.get('build') or {}).get('sha256') == cur,
                        'shot': str(d / 'shot.png') if (d / 'shot.png').exists() else None})
        routes = {k: v.get('description', '') for k, v in (self._game_ai().get('route') or {}).items()}
        return {'states_dir': str(sd), 'states': out, 'routes': routes}

    def save_state(self, name: str, description: str = '', reach: list = None, ready: dict = None, from_bug: str = None,
                   save_dir: str = None, overwrite: bool = False):
        """Save the game's current save data as a named state (work/states/<game>/<name>). The source is the save folder of the running or
        last run (from its log), a bug bundle's save/ (from_bug = bundle id), or save_dir. Only committed files are copied: make the game
        save first (in game), else the state only holds what the game saved last. reach: the route names (ai_states.toml) or step dicts that
        bring a launch from boot back to this moment; default the game's [defaults] reach (title, then load the save). ready: wait_for
        arguments that confirm the moment (default the game's). Records the build (exe SHA-256, commit), the launch's settings and env (shader
        cache, test switches) and a screenshot when the window is up. overwrite: replace a state of that name."""
        if not re.fullmatch(r'[\w.-]+', name):
            raise ValueError('name: letters, digits, "_", "-", "." only')
        dst = self._states_dir() / name
        if dst.exists() and not overwrite:
            raise RuntimeError(f'state {name!r} exists (overwrite=true replaces it)')
        bug = self._bug_dir(from_bug) if from_bug else None
        src = Path(save_dir) if save_dir else bug / 'save' if bug else self._save_dir_in_use()
        if not src.exists():
            raise RuntimeError(f'no save folder at {src}')
        ai = self._game_ai().get('defaults') or {}
        tmp = dst.with_name(dst.name + '.new')
        shutil.rmtree(tmp, ignore_errors=True)
        tmp.mkdir(parents=True)
        files = copy_save(src, tmp / 'save')
        ll = self._last_launch()
        settings = {k: v for k, v in (ll.get('settings') or {}).items() if k not in ('fs.save_dir', 'bug.dir')}
        env = {k: v for k, v in (ll.get('env') or {}).items() if not RUN_ONLY_ENV.match(k)}
        if bug:
            env = {**{k: v for k, v in self._bug_env(bug).items() if not RUN_ONLY_ENV.match(k)}, **env}
        if bug:          # the build that ran when the bundle was made
            bb = (read_json(bug / 'bug.json') or {}).get('build') or {}
            build = {'exe': bb.get('exe'), 'size': bb.get('exe_size'), 'modified': bb.get('exe_modified'), 'sha256': bb.get('exe_sha256'),
                     'commit': bb.get('commit'), 'dirty': bb.get('dirty')}
        else:
            build = {**self._build(), **self._log_build()}
        state = {'name': name, 'description': description, 'created': now_text(), 'game': self.id, 'reach': reach or ai.get('reach', []),
                 'ready': ready or ai.get('ready'), 'settings': settings, 'env': env, 'args': ll.get('args') or [], 'build': build,
                 'from_bug': from_bug, 'save_source': str(src), 'save_files': files}
        shot = None
        if bug and (bug / 'screen.png').exists():
            shutil.copy2(bug / 'screen.png', tmp / 'shot.png')
            shot = tmp / 'shot.png'
        elif self._window(required=False):
            try:
                img, _, _ = self._capture()
                if not wincap_black(img):
                    img.save(tmp / 'shot.png')
                    shot = tmp / 'shot.png'
            except Exception:                                       # noqa: BLE001  (a picture is optional)
                pass
        (tmp / 'state.json').write_text(json.dumps(state, indent=1), encoding='utf-8')
        if dst.exists():
            shutil.rmtree(dst)
        os.replace(tmp, dst)
        return {'saved': str(dst), 'save_files': files, 'from': str(src), 'reach': state['reach'], 'shot': str(dst / 'shot.png') if shot else None,
                'note': None if files else 'the save folder had no committed files: the game has not saved yet'}

    def load_state(self, name: str = None, bug: str = None, settings: dict = None, env: dict = None, args: list = None,
                   reach: bool = True, route: list = None, timeout_s: float = 900):
        """Start the game at a state: copies the state's save data (or a bug bundle's, bug = bundle id) into <run dir>/save, launches with
        [fs] save_dir there (the user's own save is never read or written), then runs the state's reach steps (e.g. boot to the title, load
        the save, wait until in game) and its ready check. Settings and env merge in this order, later wins: the game's defaults
        (ai_states.toml: shader cache, test switches), the state's (or the bundle's env.txt), this call's. reach=false: launch only.
        Returns ok, the step that failed (with the log tail) or the seconds each step took, a screenshot path, and a warning when the exe
        is not the build the state was saved with. route: steps to run instead of the state's reach (route names or step dicts, see
        save_state). A bundle's reach is that of the state its run was loaded from (bug.json "state"), else the game's default (load the
        save). Takes minutes (boot ~45 s, loading ~70 s); timeout_s bounds the whole run."""
        if bool(name) == bool(bug):
            raise ValueError('give a state name or a bug bundle id (bugs() lists them)')
        t0 = time.time()
        if bug:
            b = self._bug_dir(bug)
            meta = read_json(b / 'bug.json') or {}
            base = self._state(meta['state']) if meta.get('state') and self._has_state(meta['state']) else {}
            st = {'name': f'bug:{b.name}', 'kind': 'bug', 'dir': str(b), 'save': str(b / 'save') if (b / 'save').exists() else None,
                  'reach': base.get('reach') or (self._game_ai().get('defaults') or {}).get('reach', []), 'ready': base.get('ready'),
                  'settings': base.get('settings') or {}, 'args': base.get('args') or [],
                  'env': {**(base.get('env') or {}), **{k: v for k, v in self._bug_env(b).items() if not RUN_ONLY_ENV.match(k)}},
                  'build': meta.get('build') or {}}
        else:
            st = self._state(name)
        ai = self._game_ai().get('defaults') or {}
        if others := self.others_running():
            raise RuntimeError(f'another copy of the game runs ({others}): one at a time on this machine; wait for it')
        self.kill()                                                 # a previous run of this exe may still have the save folder open
        save = self.run_dir / 'save'
        if save.exists():
            shutil.rmtree(save)                                     # this server's own copy from the previous load_state
        if st.get('save'):
            copy_save(st['save'], save)
        else:
            save.mkdir(parents=True)
        s = {**(ai.get('settings') or {}), **(st.get('settings') or {}), **(settings or {}), 'fs.save_dir': str(save)}
        e = {**(ai.get('env') or {}), **(st.get('env') or {}), **(env or {}), 'NXRT_AI_STATE': st['name']}
        a = list(args if args is not None else st.get('args') or ai.get('args') or [])
        warn = None
        want = (st.get('build') or {}).get('sha256') or (st.get('build') or {}).get('exe_sha256')
        if want and self.exe.exists() and file_sha256(self.exe) != want:
            warn = f'the exe differs from the build this state was saved with (commit {(st.get("build") or {}).get("commit")})'
        r = self.launch(args=a, settings=s, env=e)
        out = {'state': st['name'], 'pid': r['pid'], 'log': r['log'], 'save_dir': str(save), 'save_files': count_files(save)}
        if warn:
            out['warning'] = warn
        if not reach:
            return out
        deadline = t0 + timeout_s
        res = self._run_steps(route if route is not None else st.get('reach') or [], Path(st['dir']), deadline)
        if res['ok'] and st.get('ready'):
            w = self.wait_for(**self._wait_args(st['ready'], Path(st['dir']), deadline))
            res['steps'].append({'step': 'ready', **w})
            res['ok'] = w['ok']
            if not w['ok']:
                res['failed'] = 'ready'
        out.update(res)
        out['seconds'] = round(time.time() - t0, 1)
        if self._window(required=False):
            out['shot'] = self.screenshot(show=False)['path']
        if not res['ok']:
            out['log_tail'] = self.log(25).get('lines')
        return out

    def _has_state(self, name):
        try:
            self._state(name)
            return True
        except ValueError:
            return False

    def _wait_args(self, w, base, deadline):
        w = dict(w)
        if w.get('ref') and not Path(w['ref']).is_absolute():
            w['ref'] = str(base / w['ref'])
        w['timeout_s'] = max(1.0, min(float(w.get('timeout_s', 60)), deadline - time.time()))
        return w

    def _run_steps(self, steps, base, deadline, depth=0):
        """Runs reach steps. A step is a route name or a dict with one of: route = "<name>" (ai_states.toml [route.<name>].steps),
        wait_for = {wait_for arguments}, input = [send_input steps] (posted to the window), sleep = seconds, shot = "<name>",
        press_until = {input = [...], every_s = 3, until = {wait_for arguments}, timeout_s = 120}; optional = true lets it fail."""
        routes = self._game_ai().get('route') or {}
        done = []
        for st in steps:
            st = {'route': st} if isinstance(st, str) else dict(st)
            kind = next((k for k in STEP_KINDS if k in st), None)
            t = time.time()
            if time.time() > deadline:
                return {'ok': False, 'failed': st, 'why': 'timeout_s reached', 'steps': done}
            ok, info = True, {}
            if kind == 'route':
                if st['route'] not in routes or depth > 8:
                    raise ValueError(f'unknown route {st["route"]!r} (ai_states.toml [route.<name>])')
                sub = self._run_steps(routes[st['route']].get('steps', []), base, deadline, depth + 1)
                ok, info = sub['ok'], {'steps': sub['steps'], **({'failed': sub['failed']} if not sub['ok'] else {})}
            elif kind == 'wait_for':
                info = self.wait_for(**self._wait_args(st['wait_for'], base, deadline))
                ok = info['ok']
            elif kind == 'input':
                info = self.send_input(st['input'])
            elif kind == 'sleep':
                time.sleep(max(0.0, min(float(st['sleep']), deadline - time.time())))
            elif kind == 'shot':
                info = self.screenshot(show=False, save=str(self.run_dir / 'shots' / f'{st["shot"]}.png'))
            elif kind == 'press_until':
                p = st['press_until']
                end = min(deadline, time.time() + float(p.get('timeout_s', 120)))
                from gamectl import _read_from as gamectl_read_from
                ok, presses = False, 0
                until, every = dict(p['until']), float(p.get('every_s', 3))
                rx = re.compile(until.get('pattern') or '.', re.I) if until.get('until') == 'log' else None
                lp = self._log_path()
                pos = lp.stat().st_size if lp.exists() else 0       # a log condition counts from the first press, not from each one
                line = None
                while time.time() < end and not ok:
                    if not self._pids():
                        info['why'] = 'the game is not running'
                        break
                    self.send_input(p['input'])
                    presses += 1
                    if rx:
                        t_press = time.time()
                        while not ok and time.time() - t_press < every and time.time() < end:
                            time.sleep(0.25)
                            if lp.exists():
                                lines, pos = gamectl_read_from(lp, pos)
                                line = next((ln for ln in lines if rx.search(ln)), None)
                                ok = line is not None
                    else:
                        ok = self.wait_for(**self._wait_args({**until, 'timeout_s': every}, base, end))['ok']
                info.update(presses=presses, **({'line': line} if line else {}))
            else:
                raise ValueError(f'unknown step {st!r}: one of {", ".join(STEP_KINDS)}')
            entry = {'step': st.get(kind) if kind in ('route', 'sleep', 'shot') else kind, 'ok': ok, 's': round(time.time() - t, 1)}
            entry.update({k: v for k, v in info.items() if k in ('line', 'why', 'presses', 'steps', 'failed', 'path')})
            done.append(entry)
            if not ok and not st.get('optional'):
                return {'ok': False, 'failed': st, 'steps': done}
        return {'ok': True, 'steps': done}

    # ---- bug bundles ------------------------------------------------------------------------------------------------------------------------
    def _bug_dir(self, bug):
        d = self._bugs_dir()
        if not d.exists():
            hits = []
        elif bug == 'latest':
            hits = sorted((p for p in d.iterdir() if p.is_dir()), key=lambda p: p.name)[-1:]
        elif (d / bug).is_dir():
            hits = [d / bug]
        else:
            hits = sorted(p for p in d.glob(f'*{bug}*') if p.is_dir())
        if len(hits) != 1:
            raise ValueError(f'bug bundle {bug!r}: {len(hits)} matches under {d} (bugs() lists them)')
        return hits[0]

    def _bug_env(self, b):
        env = {}
        p = Path(b) / 'env.txt'
        if p.exists():
            for ln in p.read_text(encoding='utf-8', errors='replace').splitlines():
                k, sep, v = ln.partition('=')
                if sep and k.startswith('NXRT_'):
                    env[k] = v
        return env

    def capture_bug(self, note: str = '', trace: bool = False, name: str = None, timeout_s: float = 30):
        """Save this moment as a bug bundle (work/bugs/<time>[-name]): the running game writes screen.png (the presented frame), log.txt,
        config.txt (every setting read and its source), env.txt, threads.txt (all thread stacks), save/ (committed save data), reports/
        (crash/freeze/fatal) and bug.json (build: exe SHA-256 and commit; frame, state); this side adds window.png (PrintWindow), mcp.json
        (the launch: args, settings, env, run dir) and stdout/stderr. trace: also trace the next NVN frame into the log. With no game
        running (or no answer within timeout_s) the bundle is made from the last run's files only. The player's F9 key in the game makes
        the same bundle and asks for the note in a small window. bugs() lists them; load_state(bug=...) starts from one."""
        bugs = self._bugs_dir()
        before = {p.name for p in bugs.iterdir()} if bugs.exists() else set()
        pids = self._pids()
        from gamectl import processes
        names = {p: n for p, _, n in processes()}
        targets = [p for p in pids if names.get(p, '').lower() == self.exe.name.lower()] or \
                  [p for p in pids if names.get(p, '').lower() not in ('cmd.exe', 'conhost.exe')]
        req_text = ''.join(['@trace\n' if trace else '', f'@name {name}\n' if name else '', '@origin mcp\n', note, '\n'])
        reqs = []
        made, why = None, None
        if targets:
            for p in targets:
                r = self.exe.parent / f'capture_bug.{p}'
                r.write_text(req_text, encoding='utf-8')
                reqs.append(r)
            end = time.time() + timeout_s
            while time.time() < end and made is None:
                time.sleep(0.25)
                if bugs.exists():
                    new = sorted(p for p in bugs.iterdir() if p.is_dir() and p.name not in before and (p / 'bug.json').exists())
                    made = new[-1] if new else None
            for r in reqs:
                r.unlink(missing_ok=True)                           # a target that did not take its request (a helper process)
            if made is None:
                why = f'the game did not answer the request within {timeout_s:g} s (not a build with bug capture, or stuck)'
        else:
            why = 'the game is not running'
        if made is None:
            made = self._outside_bundle(note, name, why)
        extra = []
        if not (made / 'screen.png').exists() and self._window(required=False):     # the game's own frame is better when it is there
            try:
                img, how, _ = self._capture()
                if not wincap_black(img):
                    img.save(made / 'window.png')
                    extra.append(f'window.png ({how})')
            except Exception as e:                                  # noqa: BLE001  (a picture is optional)
                extra.append(f'window.png failed: {e}')
        for f in ('stdout.txt', 'stderr.txt'):
            if (self.run_dir / f).exists():
                shutil.copy2(self.run_dir / f, made / f)
                extra.append(f)
        ll = self._last_launch()
        mcp = {'time': now_text(), 'note': note, 'run_dir': str(self.run_dir), 'last_launch': ll, 'build': self._build(),
               'running': bool(self._pids()), 'state': (ll.get('env') or {}).get('NXRT_AI_STATE')}
        (made / 'mcp.json').write_text(json.dumps(mcp, indent=1, default=str), encoding='utf-8')
        if note and not (made / 'note.txt').exists():
            (made / 'note.txt').write_text(note + '\n', encoding='utf-8')
        return {'bundle': str(made), 'id': made.name, 'by': 'game' if why is None else 'this server', **({'why': why} if why else {}),
                'files': sorted(str(p.relative_to(made)).replace('\\', '/') for p in made.iterdir()), 'added': extra}

    def _outside_bundle(self, note, name, why):
        """A bundle without the game's help: the last run's log, reports, save folder and launch."""
        stamp = time.strftime('%Y%m%d-%H%M%S')
        label = re.sub(r'[^\w-]+', '-', name or '').strip('-')[:40]
        d = self._bugs_dir() / (stamp + (f'-{label}' if label else ''))
        i = 2
        while d.exists():
            d = d.with_name(f'{stamp}{"-" + label if label else ""}-{i}')
            i += 1
        d.mkdir(parents=True)
        files = []
        if note:
            (d / 'note.txt').write_text(note + '\n', encoding='utf-8')
            files.append('note.txt')
        lp = self._log_path()
        if lp.exists():
            from gamectl import _tail
            (d / 'log.txt').write_text('\n'.join(_tail(lp, 200000, 32 << 20)) + '\n', encoding='utf-8')
            files.append('log.txt')
        for p in self._report_files():
            (d / 'reports').mkdir(exist_ok=True)
            shutil.copy2(p, d / 'reports' / p.name)
            files.append(f'reports/{p.name}')
        sv = self._save_dir_in_use()
        n = copy_save(sv, d / 'save') if sv.exists() else 0
        if n:
            files.append('save/')
        ll = self._last_launch()
        env = dict(ll.get('env') or {})
        for k, v in (ll.get('settings') or {}).items():
            env['NXRT_' + re.sub(r'\W', '_', k.upper().replace('.', '_', 1))] = ('1' if v else '0') if isinstance(v, bool) else str(v)
        (d / 'env.txt').write_text(''.join(f'{k}={v}\n' for k, v in env.items() if k.startswith(('NXRT_', 'SDL_'))), encoding='utf-8')
        files.append('env.txt')
        b = self._build()
        meta = {'format': 1, 'complete': True, 'time': now_text(), 'origin': 'mcp (no game)', 'why': why,
                'build': {'exe': b.get('exe'), 'exe_size': b.get('size'), 'exe_modified': b.get('modified'), 'exe_sha256': b.get('sha256'),
                          **self._log_build()},
                'state': env.get('NXRT_AI_STATE', ''), 'save_dir': str(sv), 'save_files': n, 'log': str(lp), 'screenshot': False, 'files': files}
        (d / 'bug.json').write_text(json.dumps(meta, indent=1), encoding='utf-8')
        return d

    def bugs(self, id: str = None, tail: int = 40, show: bool = True):
        """Bug bundles (work/bugs), newest first: id (folder name), time, origin (hotkey = the player's F9, mcp, request), the note's first
        line, the state the run was loaded from, screenshot and save files present, the build commit and whether the exe on disk is the same
        build. With id (a folder name, part of one, or "latest"): the whole bug.json, the note, the files, the last `tail` lines of log.txt,
        and (show) the screenshot. load_state(bug=id) starts the game from the bundle's save data with its settings."""
        d = self._bugs_dir()
        if id is None:
            cur = file_sha256(self.exe) if self.exe.exists() else None
            out = []
            for b in sorted((p for p in d.iterdir() if p.is_dir()), key=lambda p: p.name, reverse=True) if d.exists() else []:
                m = read_json(b / 'bug.json') or {}
                note = (b / 'note.txt').read_text(encoding='utf-8', errors='replace').strip().splitlines() if (b / 'note.txt').exists() else []
                out.append({'id': b.name, 'time': m.get('time'), 'origin': m.get('origin'), 'complete': bool(m.get('complete')),
                            'note': note[0][:200] if note else '', 'state': m.get('state') or None,
                            'screenshot': (b / 'screen.png').exists() or (b / 'window.png').exists(), 'save_files': count_files(b / 'save'),
                            'commit': (m.get('build') or {}).get('commit'), 'same_exe': (m.get('build') or {}).get('exe_sha256') == cur})
            return {'bugs_dir': str(d), 'bugs': out}
        b = self._bug_dir(id)
        info = {'id': b.name, 'path': str(b), 'bug': read_json(b / 'bug.json'), 'mcp': read_json(b / 'mcp.json'),
                'note': (b / 'note.txt').read_text(encoding='utf-8', errors='replace').strip() if (b / 'note.txt').exists() else '',
                'files': sorted(str(p.relative_to(b)).replace('\\', '/') for p in b.rglob('*') if p.is_file())[:200]}
        if (b / 'log.txt').exists():
            from gamectl import _tail
            info['log_tail'] = [ln[:400] for ln in _tail(b / 'log.txt', tail)]
        pic = next((b / f for f in ('screen.png', 'window.png') if (b / f).exists()), None)
        if show and pic:
            return jpeg(pic), info
        return info
