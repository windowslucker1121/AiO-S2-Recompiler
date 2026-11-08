"""Saved states and bug bundles for gamectl.Game: the tools states, save_state, load_state, capture_bug, bugs (a mixin of Game).

A state is not a process snapshot (a static recompiler's guest threads live on host stacks of lifted C++ frames, next to kernel, service
and Vulkan objects that cannot be written out and recreated). It is what the game itself can restore plus the way back to the moment:
  work/states/<game>/<name>/state.json   name, description, created, build (exe size/time/SHA-256, commit), settings, env, reach, ready
                           save/          the game's committed save data (copied from [fs] save_dir, never from or into the user's own)
                           shot.png       the window when the state was saved
load_state copies save/ into the instance's save folder (<run dir>[/<instance>]/save), launches the exe with [fs] save_dir there, then runs the reach steps (boot to the title,
load the save, wait until in game) and the ready check. Built-in states (no save of their own, e.g. a fresh New Game) and the routes
come from recomp/games/<id>/ai_states.toml: the game knowledge stays in the game's folder.

Bug bundles (runtime nxrt/bugcapture.h) are folders under work/bugs: the F9 hotkey in the game writes one, capture_bug asks the running
game for one through a request file (<request dir>/capture_bug.<pid>: the instance folder, or the exe's folder for the default run) and
adds what only this side has (the launch arguments, stdout/stderr); with no game running it writes the bundle itself from the last run's
files. States and bundles record the instance they came from (several runs at once, gamectl "Several runs at once").
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
    """Methods of gamectl.Game (it provides id, exe, run_dir, runs, _target, _run_dir, _log_path, _capture, launch, wait_for, ...).
    Every tool takes instance= (and the ones acting on a running game pid=) like the rest of the server: a state or bundle records the
    instance it came from, and load_state starts the same instance again unless told otherwise."""

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

    def _log_head(self, t, rx):
        """The first match of rx in the first 400 lines of the target's log (the start-up lines), or None."""
        lp = self._log_path(t)
        if lp and lp.exists():
            with open(lp, encoding='utf-8', errors='replace') as f:
                for i, ln in enumerate(f):
                    if m := rx.search(ln):
                        return m
                    if i > 400:
                        break
        return None

    def _log_build(self, t):
        """The commit the target's exe was built from, from its log line "boot: built from commit <sha>[ with uncommitted changes]"."""
        m = self._log_head(t, re.compile(r'built from commit (\w*)( with uncommitted changes)?'))
        return {'commit': m.group(1), 'dirty': bool(m.group(2))} if m else {}

    def _inst(self, t, instance=None):
        from gamectl import DEFAULT, instance_name
        return t.instance if t else (instance_name(instance) or DEFAULT)

    def _last_launch(self, t, instance=None):
        """The launch arguments of the target's run: this server's, else <run dir of the instance>/launch.json (a CLI call is a new
        process). Another session's run: {} (its launch is not known here)."""
        if t and t.own and t.own.last_launch:
            return t.own.last_launch
        if t and not t.own:
            return {}
        return read_json(self._run_dir(self._inst(t, instance)) / 'launch.json') or {}

    def _save_dir_in_use(self, t, instance=None):
        """The target's save folder: its log line "save data in '...'", else its launch settings, else <run dir of the instance>/save."""
        if m := self._log_head(t, re.compile(r"save data in '([^']+)'")):
            return Path(m.group(1))
        s = self._last_launch(t, instance).get('settings') or {}
        return Path(s['fs.save_dir']) if s.get('fs.save_dir') else self._run_dir(self._inst(t, instance)) / 'save'

    def _picture(self, t):
        """The target's window as a picture (the game's own frame first), or None: never a black one."""
        try:
            if t and t.running and self._window(t, required=False):
                img, how, _ = self._capture(t)
                if not wincap_black(img):
                    return img, how
        except Exception:                                           # noqa: BLE001  (a picture is optional)
            pass
        return None

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

    def _has_state(self, name):
        try:
            self._state(name)
            return True
        except ValueError:
            return False

    def states(self):
        """The states load_state can start from: saved ones (work/states/<game>/<name>: the game's save data + how to get back there) and
        built-in ones of the game (recomp/games/<id>/ai_states.toml: no save of their own, e.g. a fresh New Game up to the first scene).
        Per state: description, reach (the route names run after the launch), ready check, save files, created, the instance it was saved
        from (load_state starts that instance unless told otherwise), and whether the exe on disk is the build it was saved with (same
        SHA-256). Also the routes the game defines."""
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
                        'instance': s.get('instance', 'default'), 'reach': s.get('reach', []), 'save_files': count_files(d / 'save'),
                        'from_bug': s.get('from_bug'), 'build_commit': (s.get('build') or {}).get('commit'),
                        'same_exe': (s.get('build') or {}).get('sha256') == cur, 'shot': str(d / 'shot.png') if (d / 'shot.png').exists() else None})
        routes = {k: v.get('description', '') for k, v in (self._game_ai().get('route') or {}).items()}
        return {'states_dir': str(sd), 'states': out, 'routes': routes}

    def save_state(self, name: str, description: str = '', reach: list = None, ready: dict = None, from_bug: str = None,
                   save_dir: str = None, overwrite: bool = False, instance: str = None, pid: int = None):
        """Save the game's current save data as a named state (work/states/<game>/<name>). The source is the save folder of a run
        (instance= or pid= choose it, as in status; its log names the folder; a run that has ended still counts), a bug bundle's save/
        (from_bug = bundle id), or save_dir. Only committed files are copied: make the game save first (in game), else the state only holds
        what the game saved last. reach: the route names (ai_states.toml) or step dicts that bring a launch from boot back to this moment;
        default the game's [defaults] reach (title, then load the save). ready: wait_for arguments that confirm the moment (default the
        game's). Records the instance, the build (exe SHA-256, commit), the launch's settings and env (shader cache, test switches) and a
        screenshot when the window is up. overwrite: replace a state of that name."""
        if not re.fullmatch(r'[\w.-]+', name):
            raise ValueError('name: letters, digits, "_", "-", "." only')
        dst = self._states_dir() / name
        if dst.exists() and not overwrite:
            raise RuntimeError(f'state {name!r} exists (overwrite=true replaces it)')
        bug = self._bug_dir(from_bug) if from_bug else None
        t = None if bug or save_dir else self._target(instance, pid, required=False)
        src = Path(save_dir) if save_dir else bug / 'save' if bug else self._save_dir_in_use(t, instance)
        if not src.exists():
            raise RuntimeError(f'no save folder at {src}')
        ai = self._game_ai().get('defaults') or {}
        tmp = dst.with_name(dst.name + '.new')
        shutil.rmtree(tmp, ignore_errors=True)
        tmp.mkdir(parents=True)
        files = copy_save(src, tmp / 'save')
        if bug:          # the run the bundle came from: its environment, instance and build
            meta = read_json(bug / 'bug.json') or {}
            ll, inst = {}, meta.get('instance') or 'default'
            env = {k: v for k, v in self._bug_env(bug).items() if not RUN_ONLY_ENV.match(k)}
            bb = meta.get('build') or {}
            build = {'exe': bb.get('exe'), 'size': bb.get('exe_size'), 'modified': bb.get('exe_modified'), 'sha256': bb.get('exe_sha256'),
                     'commit': bb.get('commit'), 'dirty': bb.get('dirty')}
        else:
            ll, inst = self._last_launch(t, instance), self._inst(t, instance)
            env = {k: v for k, v in (ll.get('env') or {}).items() if not RUN_ONLY_ENV.match(k)}
            build = {**self._build(), **self._log_build(t)}
        settings = {k: v for k, v in (ll.get('settings') or {}).items() if k not in ('fs.save_dir', 'bug.dir')}
        state = {'name': name, 'description': description, 'created': now_text(), 'game': self.id, 'instance': inst,
                 'reach': reach or ai.get('reach', []), 'ready': ready or ai.get('ready'), 'settings': settings, 'env': env,
                 'args': ll.get('args') or [], 'build': build, 'from_bug': from_bug, 'save_source': str(src), 'save_files': files}
        shot = None
        if bug and (bug / 'screen.png').exists():
            shutil.copy2(bug / 'screen.png', tmp / 'shot.png')
            shot = tmp / 'shot.png'
        elif pic := self._picture(t):
            pic[0].save(tmp / 'shot.png')
            shot = tmp / 'shot.png'
        (tmp / 'state.json').write_text(json.dumps(state, indent=1), encoding='utf-8')
        if dst.exists():
            shutil.rmtree(dst)
        os.replace(tmp, dst)
        return {'saved': str(dst), 'instance': inst, 'save_files': files, 'from': str(src), 'reach': state['reach'],
                'shot': str(dst / 'shot.png') if shot else None,
                'note': None if files else 'the save folder had no committed files: the game has not saved yet'}

    def load_state(self, name: str = None, bug: str = None, settings: dict = None, env: dict = None, args: list = None,
                   reach: bool = True, route: list = None, timeout_s: float = 900, instance: str = None):
        """Start the game at a state: copies the state's save data (or a bug bundle's, bug = bundle id) into the instance's own save folder
        (<run dir>/save, or <run dir>/<instance>/save), launches with [fs] save_dir there (the player's own save is never read or written),
        then runs the state's reach steps (e.g. boot to the title, load the save, wait until in game) and its ready check. instance: the
        run to start (default: the instance the state or bundle came from); this server's previous run of it is ended first, another
        session's run of that name is never touched (refused: pass another instance). Settings and env merge in this order, later wins:
        the game's defaults (ai_states.toml: shader cache, test switches), the state's (or the bundle's env.txt), this call's. reach=false:
        launch only. route: steps to run instead of the state's reach (route names or step dicts, see save_state). A bundle's reach is that
        of the state its run was loaded from (bug.json "state"), else the game's default (load the save). Returns ok, the step that failed
        (with the log tail) or the seconds each step took, a screenshot path, and a warning when the exe is not the build the state was
        saved with. Takes a minute or more (boot, loading); timeout_s bounds the whole run."""
        from gamectl import DEFAULT, instance_name
        if bool(name) == bool(bug):
            raise ValueError('give a state name or a bug bundle id (bugs() lists them)')
        t0 = time.time()
        if bug:
            b = self._bug_dir(bug)
            meta = read_json(b / 'bug.json') or {}
            base = self._state(meta['state']) if meta.get('state') and self._has_state(meta['state']) else {}
            st = {'name': f'bug:{b.name}', 'kind': 'bug', 'dir': str(b), 'save': str(b / 'save') if (b / 'save').exists() else None,
                  'instance': meta.get('instance') or DEFAULT,
                  'reach': base.get('reach') or (self._game_ai().get('defaults') or {}).get('reach', []), 'ready': base.get('ready'),
                  'settings': base.get('settings') or {}, 'args': base.get('args') or [],
                  'env': {**(base.get('env') or {}), **{k: v for k, v in self._bug_env(b).items() if not RUN_ONLY_ENV.match(k)}},
                  'build': meta.get('build') or {}}
        else:
            st = self._state(name)
        inst = instance_name(instance) or instance_name(st.get('instance')) or DEFAULT
        ai = self._game_ai().get('defaults') or {}
        if clash := [x for x in self._running() if x.instance == inst and not x.own]:
            raise RuntimeError(f'instance {inst} is already running as {clash[0].label()}: pass instance="<another name>"')
        if (run := self.runs.get(inst)) and run.alive():
            self._kill_run(run)                                     # it may still have the save folder open
        save = self._run_dir(inst) / 'save'
        if save.exists():
            shutil.rmtree(save)                                     # this server's own copy from the previous load_state
        if st.get('save'):
            copy_save(st['save'], save)
        else:
            save.mkdir(parents=True)
        s = {**(ai.get('settings') or {}), **(st.get('settings') or {}), **(settings or {}), 'fs.save_dir': str(save)}
        e = {**(ai.get('env') or {}), **(st.get('env') or {}), **(env or {}), 'NXRT_AI_STATE': st['name']}
        a = list(args if args is not None else st.get('args') or ai.get('args') or [])
        a = [x for i, x in enumerate(a) if x not in ('--instance', '--instance-dir') and (i == 0 or a[i - 1] not in ('--instance', '--instance-dir'))]
        warn = None
        want = (st.get('build') or {}).get('sha256') or (st.get('build') or {}).get('exe_sha256')
        if want and self.exe.exists() and file_sha256(self.exe) != want:
            warn = f'the exe differs from the build this state was saved with (commit {(st.get("build") or {}).get("commit")})'
        r = self.launch(args=a, settings=s, env=e, instance=inst)
        out = {'state': st['name'], 'instance': inst, 'pid': r['pid'], 'log': r['log'], 'save_dir': str(save), 'save_files': count_files(save)}
        if warn:
            out['warning'] = warn
        if 'exit_code' in r:
            return {**out, 'ok': False, 'failed': 'launch', 'exit_code': r['exit_code'], 'meaning': r.get('meaning'), 'stderr': r.get('stderr')}
        if not reach:
            return out
        deadline = t0 + timeout_s
        res = self._run_steps(route if route is not None else st.get('reach') or [], Path(st['dir']), deadline, inst)
        if res['ok'] and st.get('ready'):
            w = self.wait_for(**self._wait_args(st['ready'], Path(st['dir']), deadline, inst))
            res['steps'].append({'step': 'ready', **w})
            res['ok'] = w['ok']
            if not w['ok']:
                res['failed'] = 'ready'
        out.update(res)
        out['seconds'] = round(time.time() - t0, 1)
        t = self._target(inst, required=False)
        if pic := self._picture(t):
            pic[0].save(p := self._shot_path(t, 'state'))
            out['shot'] = str(p)
        if not res['ok']:
            out['log_tail'] = self.log(25, instance=inst).get('lines')
        return out

    def _wait_args(self, w, base, deadline, inst):
        w = dict(w)
        if w.get('ref') and not Path(w['ref']).is_absolute():
            w['ref'] = str(base / w['ref'])
        w['timeout_s'] = max(1.0, min(float(w.get('timeout_s', 60)), deadline - time.time()))
        w['instance'] = inst
        return w

    def _run_steps(self, steps, base, deadline, inst, depth=0):
        """Runs reach steps on the instance inst. A step is a route name or a dict with one of: route = "<name>" (ai_states.toml
        [route.<name>].steps), wait_for = {wait_for arguments}, input = [send_input steps] (posted to the window), sleep = seconds,
        shot = "<name>", press_until = {input = [...], every_s = 3, until = {wait_for arguments}, timeout_s = 120}; optional = true lets
        it fail."""
        from gamectl import _read_from
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
                sub = self._run_steps(routes[st['route']].get('steps', []), base, deadline, inst, depth + 1)
                ok, info = sub['ok'], {'steps': sub['steps'], **({'failed': sub['failed']} if not sub['ok'] else {})}
            elif kind == 'wait_for':
                info = self.wait_for(**self._wait_args(st['wait_for'], base, deadline, inst))
                ok = info['ok']
            elif kind == 'input':
                info = self.send_input(st['input'], instance=inst)
            elif kind == 'sleep':
                time.sleep(max(0.0, min(float(st['sleep']), deadline - time.time())))
            elif kind == 'shot':
                info = self.screenshot(show=False, save=str(self._run_dir(inst) / 'shots' / f'{st["shot"]}.png'), instance=inst)
            elif kind == 'press_until':
                p = st['press_until']
                end = min(deadline, time.time() + float(p.get('timeout_s', 120)))
                ok, presses, line = False, 0, None
                until, every = dict(p['until']), float(p.get('every_s', 3))
                rx = re.compile(until.get('pattern') or '.', re.I) if until.get('until') == 'log' else None
                tg = self._target(inst)
                lp = self._log_path(tg)
                pos = lp.stat().st_size if lp and lp.exists() else 0     # a log condition counts from the first press, not from each one
                while time.time() < end and not ok:
                    if not self._alive(tg):
                        info['why'] = 'the game is not running'
                        break
                    self.send_input(p['input'], instance=inst)
                    presses += 1
                    if rx:
                        t_press = time.time()
                        while not ok and time.time() - t_press < every and time.time() < end:
                            time.sleep(0.25)
                            if lp and lp.exists():
                                lines, pos = _read_from(lp, pos)
                                line = next((ln for ln in lines if rx.search(ln)), None)
                                ok = line is not None
                    else:
                        ok = self.wait_for(**self._wait_args({**until, 'timeout_s': every}, base, end, inst))['ok']
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

    def capture_bug(self, note: str = '', trace: bool = False, name: str = None, timeout_s: float = 30, instance: str = None, pid: int = None):
        """Save this moment of a run (instance= or pid= choose it, as in status) as a bug bundle (work/bugs/<time>[-instance][-name]): the
        running game writes screen.png (the presented frame), log.txt, config.txt (every setting read and its source), env.txt,
        threads.txt (all thread stacks), save/ (committed save data), reports/ (crash/freeze/fatal) and bug.json (build: exe SHA-256 and
        commit; instance, frame, state); this side adds window.png (only when the game gave no frame), mcp.json (the launch: args,
        settings, env, run dir) and stdout/stderr. trace: also trace the next NVN frame into the log. With the run not running (or no answer
        within timeout_s) the bundle is made from its last files only. The player's F9 key in the game makes the same bundle and asks for
        the note in a small window. bugs() lists them; load_state(bug=...) starts from one (in the same instance)."""
        t = self._target(instance, pid, required=False)
        inst = self._inst(t, instance)
        bugs = self._bugs_dir()
        before = {p.name for p in bugs.iterdir()} if bugs.exists() else set()
        made, why = None, None
        if t and t.running and (d := self._request_dir(t)) and (targets := self._request_pids(t)):
            from gamectl import _read_from
            lp = self._log_path(t)
            pos = lp.stat().st_size if lp and lp.exists() else 0
            req_text = ''.join(['@trace\n' if trace else '', f'@name {name}\n' if name else '', '@origin mcp\n', note, '\n'])
            reqs = []
            for p in targets:
                r = Path(d) / f'capture_bug.{p}'
                r.write_text(req_text, encoding='utf-8')
                reqs.append(r)
            end = time.time() + timeout_s
            done_rx = re.compile(r'bug: bundle complete (.+?) \(\d+ save files')
            while time.time() < end and made is None:
                time.sleep(0.25)
                if lp and lp.exists():                              # the run's own log names its bundle (it may use another bug folder)
                    lines, pos = _read_from(lp, pos)
                    if m := next((done_rx.search(ln) for ln in lines if done_rx.search(ln)), None):
                        made = Path(m.group(1))
                if made is None and bugs.exists():
                    new = sorted(p for p in bugs.iterdir() if p.is_dir() and p.name not in before and (p / 'bug.json').exists())
                    made = new[-1] if new else None
            for r in reqs:
                r.unlink(missing_ok=True)                           # a target that did not take its request
            if made is None:
                why = f'the game did not answer the request within {timeout_s:g} s (not a build with bug capture, or stuck)'
        else:
            why = f'instance {inst} is not running'
        if made is None:
            made = self._outside_bundle(note, name, why, t, inst)
        extra = []
        if not (made / 'screen.png').exists() and (pic := self._picture(t)):     # the game's own frame is better when it is there
            pic[0].save(made / 'window.png')
            extra.append(f'window.png ({pic[1]})')
        rd = t.own.run_dir if t and t.own else self._run_dir(inst) if not t else None
        for f in ('stdout.txt', 'stderr.txt'):
            if rd and (rd / f).exists():
                shutil.copy2(rd / f, made / f)
                extra.append(f)
        ll = self._last_launch(t, instance)
        mcp = {'time': now_text(), 'note': note, 'instance': inst, 'pid': t.pid if t else None, 'run_dir': str(rd) if rd else None,
               'last_launch': ll, 'build': self._build(), 'running': bool(t and t.running), 'state': (ll.get('env') or {}).get('NXRT_AI_STATE')}
        (made / 'mcp.json').write_text(json.dumps(mcp, indent=1, default=str), encoding='utf-8')
        if note and not (made / 'note.txt').exists():
            (made / 'note.txt').write_text(note + '\n', encoding='utf-8')
        return {'bundle': str(made), 'id': made.name, 'instance': inst, 'by': 'game' if why is None else 'this server',
                **({'why': why} if why else {}), 'files': sorted(str(p.relative_to(made)).replace('\\', '/') for p in made.iterdir()),
                'added': extra}

    def _outside_bundle(self, note, name, why, t, inst):
        """A bundle without the game's help: the run's last log, reports, save folder and launch."""
        from gamectl import DEFAULT, _tail
        stamp = time.strftime('%Y%m%d-%H%M%S')
        label = '-'.join(x for x in ([inst] if inst != DEFAULT else []) + [re.sub(r'[^\w-]+', '-', name or '').strip('-')[:40]] if x)
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
        if t is None:
            t = self._target(inst, required=False)
        lp = self._log_path(t) if t else self._run_dir(inst) / self.log_name
        if lp and lp.exists():
            (d / 'log.txt').write_text('\n'.join(_tail(lp, 200000, 32 << 20)) + '\n', encoding='utf-8')
            files.append('log.txt')
        for p in self._report_files(t) if t else []:
            (d / 'reports').mkdir(exist_ok=True)
            shutil.copy2(p, d / 'reports' / p.name)
            files.append(f'reports/{p.name}')
        sv = self._save_dir_in_use(t, inst)
        n = copy_save(sv, d / 'save') if sv.exists() else 0
        if n:
            files.append('save/')
        ll = self._last_launch(t, inst)
        env = dict(ll.get('env') or {})
        for k, v in (ll.get('settings') or {}).items():
            env['NXRT_' + re.sub(r'\W', '_', k.upper().replace('.', '_', 1))] = ('1' if v else '0') if isinstance(v, bool) else str(v)
        (d / 'env.txt').write_text(''.join(f'{k}={v}\n' for k, v in env.items() if k.startswith(('NXRT_', 'SDL_'))), encoding='utf-8')
        files.append('env.txt')
        b = self._build()
        meta = {'format': 1, 'complete': True, 'time': now_text(), 'origin': 'mcp (no game)', 'why': why, 'instance': inst,
                'build': {'exe': b.get('exe'), 'exe_size': b.get('size'), 'exe_modified': b.get('modified'), 'exe_sha256': b.get('sha256'),
                          **self._log_build(t)},
                'state': env.get('NXRT_AI_STATE', ''), 'save_dir': str(sv), 'save_files': n, 'log': str(lp), 'screenshot': False, 'files': files}
        (d / 'bug.json').write_text(json.dumps(meta, indent=1), encoding='utf-8')
        return d

    # ---- frame captures (runtime/docs/CAPTURE.md) ------------------------------------------------------------------------------------------
    def _captures_dir(self):
        """Where frame captures go: NXRT_AI_CAPTURES_DIR, else work/captures of the checkout (the development exe's [capture] dir default)."""
        return Path(os.environ.get('NXRT_AI_CAPTURES_DIR') or self._root() / 'work' / 'captures').resolve()

    def capture_frame(self, name: str = '', timeout_s: float = 120, instance: str = None, pid: int = None):
        """Save the next whole frame of a running game (instance= or pid= choose it) as a frame capture (work/captures/<time>[-instance][-name]):
        the frame's NVN command lists, every object they reach, the GPU state it starts from, the guest memory the GPU reads, the images the
        GPU rendered before it, and the game's own picture of it (reference.png). Takes a few seconds (the game holds its frame meanwhile) and
        3-4 GB of disk. replay_capture shows it again without the game; captures lists them."""
        t = self._target(instance, pid, required=True)
        d, targets = self._request_dir(t), self._request_pids(t)
        if not (t and t.running and d and targets):
            return {'ok': False, 'why': 'the run is not running'}
        from gamectl import _read_from
        lp = self._log_path(t)
        pos = lp.stat().st_size if lp and lp.exists() else 0
        reqs = []
        for p in targets:
            r = Path(d) / f'capture_frame.{p}'
            r.write_text((name or '') + '\n', encoding='utf-8')
            reqs.append(r)
        done = re.compile(r'nvn\.capture: (written|FAILED) (.+?): (.*)$')
        end = time.time() + timeout_s
        while time.time() < end:
            time.sleep(0.5)
            if lp and lp.exists():
                lines, pos = _read_from(lp, pos)
                for ln in lines:
                    if m := done.search(ln):
                        for r in reqs:
                            r.unlink(missing_ok=True)
                        out = {'ok': m.group(1) == 'written', 'capture': m.group(2), 'id': Path(m.group(2)).name, 'summary': m.group(3)}
                        if out['ok']:
                            out['removed'] = self._prune_captures(keep_id=out['id'])
                        return out
        for r in reqs:
            r.unlink(missing_ok=True)
        return {'ok': False, 'why': f'no capture within {timeout_s:g} s (a build without frame capture, no frame presented, or stuck)'}

    def _capture_list(self):
        d = self._captures_dir()
        return d, sorted((p for p in d.iterdir() if p.is_dir() and (p / 'capture.bin').exists()), key=lambda p: p.name, reverse=True) if d.exists() else []

    def _prune_captures(self, keep_id: str = None):
        """After a new capture: only the newest NXRT_AI_CAPTURES_KEEP captures (default 5) stay, plus every capture marked keep (a file
        named `keep` in its folder: captures(id, keep=True)). Captures are 3-4 GB each. Returns the ids removed."""
        try:
            n = int(os.environ.get('NXRT_AI_CAPTURES_KEEP', '5'))
        except ValueError:
            n = 5
        _, caps = self._capture_list()
        removed = []
        for c in [c for c in caps if not (c / 'keep').exists()][max(n, 1):]:
            if c.name == keep_id:
                continue
            shutil.rmtree(c, ignore_errors=True)
            removed.append(c.name)
        return removed

    def captures(self, id: str = None, show: bool = True, keep: bool = None):
        """Frame captures (work/captures), newest first: id, size in GB, whether the game's reference picture is there, whether it is marked
        keep. With id (a folder name, part of one, or "latest"): the files and (show) the reference picture; keep=true marks it so that
        capture_frame never removes it (it keeps only the newest NXRT_AI_CAPTURES_KEEP unmarked captures, default 5), keep=false unmarks it."""
        d, caps = self._capture_list()
        if id is None:
            return {'captures_dir': str(d), 'captures': [{'id': c.name, 'gb': round(sum(f.stat().st_size for f in c.iterdir() if f.is_file()) / 2**30, 2),
                                                          'reference': (c / 'reference.png').exists(), 'keep': (c / 'keep').exists()} for c in caps]}
        c = caps[0] if id == 'latest' and caps else next((x for x in caps if x.name == id), None) or next((x for x in caps if id in x.name), None)
        if c is None:
            raise ValueError(f'no capture {id!r} in {d}')
        if keep is True:
            (c / 'keep').write_text('kept: capture_frame does not remove this capture\n', encoding='utf-8')
        elif keep is False:
            (c / 'keep').unlink(missing_ok=True)
        info = {'id': c.name, 'path': str(c), 'keep': (c / 'keep').exists(), 'files': {f.name: f.stat().st_size for f in c.iterdir() if f.is_file()}}
        if show and (c / 'reference.png').exists():
            return jpeg(c / 'reference.png'), info
        return info

    def replay_capture(self, id: str = 'latest', instance: str = 'replay', seconds: float = 0, settings: dict = None, env: dict = None):
        """Show a frame capture without the game: the exe restores the captured memory, objects and images and replays the frame in a loop
        (each loop from the captured state; settings {"capture.reset": 0} lets loops build on each other). Starts in a few seconds. Every
        back-end debug key works on it ([nvn] skip_fs_file, trace_frame, trace_snap*, the F1 overlay); screenshot/compare work as on the
        game. id: a capture (captures), seconds: end after that long (0: until killed). Takes about 6-10 GB of memory."""
        c = self.captures(id, show=False)['path']
        args = ['--replay-capture', c] + (['--max-seconds', str(int(seconds))] if seconds else [])
        return self.launch(args=args, settings=settings, env=env, instance=instance)

    def bugs(self, id: str = None, tail: int = 40, show: bool = True, instance: str = None):
        """Bug bundles (work/bugs), newest first: id (folder name), time, origin (hotkey = the player's F9, mcp, request), the instance it
        came from, the note's first line, the state the run was loaded from, screenshot and save files present, the build commit and whether
        the exe on disk is the same build; instance= keeps one instance's. With id (a folder name, part of one, or "latest"): the whole
        bug.json, the note, the files, the last `tail` lines of log.txt, and (show) the screenshot. load_state(bug=id) starts the game from
        the bundle's save data with its settings, in its instance."""
        from gamectl import instance_name
        d = self._bugs_dir()
        if id is None:
            want = instance_name(instance)
            cur = file_sha256(self.exe) if self.exe.exists() else None
            out = []
            for b in sorted((p for p in d.iterdir() if p.is_dir()), key=lambda p: p.name, reverse=True) if d.exists() else []:
                m = read_json(b / 'bug.json') or {}
                inst = m.get('instance') or 'default'
                if want and inst != want:
                    continue
                note = (b / 'note.txt').read_text(encoding='utf-8', errors='replace').strip().splitlines() if (b / 'note.txt').exists() else []
                out.append({'id': b.name, 'time': m.get('time'), 'origin': m.get('origin'), 'instance': inst, 'complete': bool(m.get('complete')),
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
