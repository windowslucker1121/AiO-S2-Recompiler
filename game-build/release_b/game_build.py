# SPDX-License-Identifier: GPL-3.0-or-later
"""First-run game build of release model (b): turns the player's own dump into the game exe on their PC.

    python -I tools/build/game_build.py --exefs <dump>/updates/<v>/exefs --game <output folder> [--jobs N] [--keep]
                                        [--manifest games/<id>/game.toml] [--sdk games/<id>/sdk] [--runtime games/<id>/runtime]

Game independent: everything about the game (id, exe and runtime DLL names, supported dump hashes and build ID, modules with their
load addresses) comes from the game manifest, game.toml in the package root (two folders above this script) unless --manifest says
otherwise. The package ships no game code. This script
  1. verifies the dump's ExeFS against the manifest (the supported build: sha256 of every file + main's build ID),
     and writes plain copies of ZBIC-compressed NSOs (Switch 2 main modules) with the bundled nx_plain_nso.exe,
  2. lifts the NSOs with the bundled lifter (recomp/nxrecomp) into <game>/build/lift,
  3. compiles the generated C++ with the bundled Zig (zig c++, mingw target) into <game>/build/obj, in parallel,
  4. links the exe = sdk/stub.cpp + the lifted objects + sdk/<runtime stem>.lib (the runtime, the DLL of the package),
  5. copies the runtime files (the runtime DLL, the ASTC decoder DLLs, fonts/) of --runtime next to the exe,
  6. deletes the generated sources and objects (unless --keep) and records the result in <game>/build/done.json.

Resumable: a module's lift counts once its marker file is written, an object once it is renamed into place, so a stopped
build continues where it stopped. A file named <game>/build/cancel stops it (the launcher's Pause).
Output for the launcher, one per line on stdout:  @progress <phase> <done> <total> <text>  /  @error <text>
Exit codes: 0 done, 1 error, 2 cancelled.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, FIRST_COMPLETED, wait

HERE = os.path.dirname(os.path.abspath(__file__))
RECOMP = os.path.join(HERE, 'recomp')
SDK = os.path.join(HERE, 'sdk')                # --sdk: stub.cpp, rt_api.h, game.rc, game.manifest.in and <runtime stem>.lib of the game
RUNTIME = ''                                   # --runtime: the files the exe runs with (copied next to it)
ZIG = os.path.join(HERE, 'zig', 'zig.exe')
PLAIN_NSO = os.path.join(HERE, 'nx_plain_nso.exe')
FORMAT = 2          # bump when the build layout or the runtime API (sdk/rt_api.h) changes: older partial builds are then started over
                    # (the launcher also treats an exe whose done.json has another format as not built: gamebuild.rs BUILD_FORMAT)

MANIFEST = {}       # the game manifest (load_manifest)
MANIFEST_PATH = ''
GAME_ID = ''
EXE_NAME = ''
RT_LIB = ''
MODULES = []        # (name, NSO file, guest base, NxModule.index, translation units, sites file or None), in lifter order
GENERIC = False     # the manifest says generic = true: an experimental build of a title without a port (make_manifest.py wrote it from the
                    # player's dump; the generic runtime DLL reads it next to the exe at start, the lifter uses its neutral config)


def load_manifest(path):
    global MANIFEST, GAME_ID, EXE_NAME, RT_LIB, MODULES, GENERIC, MANIFEST_PATH
    import tomllib
    with open(path, 'rb') as f:
        MANIFEST = tomllib.load(f)
    MANIFEST_PATH = os.path.abspath(path)
    g = MANIFEST['game']
    GAME_ID = g['id']
    GENERIC = bool(g.get('generic'))
    EXE_NAME = MANIFEST['files']['exe']
    RT_LIB = os.path.splitext(MANIFEST['files']['runtime_dll'])[0] + '.lib'
    MODULES = [(m['name'], m['file'], m['base'], m['index'], m['units'], m.get('sites')) for m in MANIFEST['module']]


# recomp/cmake/NxRecomp.cmake flags for the lifted code, in clang (not clang-cl) syntax
CXXFLAGS = ['-target', 'x86_64-windows-gnu', '-mcpu=x86_64+cx16', '-std=c++20', '-O1', '-w', '-fno-exceptions', '-fno-rtti',
            '-fno-strict-aliasing', '-ffp-contract=off', '-fno-threadsafe-statics']
CREATE_NO_WINDOW = 0x08000000
BELOW_NORMAL = 0x00004000


def say(kind, *parts):
    print('@' + kind + ' ' + ' '.join(str(p) for p in parts), flush=True)


def progress(phase, done, total, text):
    say('progress', phase, done, total, text)


class Fail(Exception):
    pass


class Cancelled(Exception):
    pass


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def verify(exefs):
    d = MANIFEST['dump']
    want = {'version': d['version'], 'main_build_id': d['main_build_id'], 'sha256': {f['name']: f['sha256'] for f in d['file']}}
    missing = [n for n in want['sha256'] if not os.path.isfile(os.path.join(exefs, n))]
    if missing:
        raise Fail('The game files folder %s is missing %s. Pick the exefs of a complete dump.' % (exefs, ', '.join(missing)))
    with open(os.path.join(exefs, 'main'), 'rb') as f:
        head = f.read(0x60)
    if head[:4] != b'NSO0':
        raise Fail('exefs/main is not a decrypted NSO. Extract the game files with your own keys first (decrypted exefs and romfs).')
    bid = head[0x40:0x54].hex()
    if bid != want['main_build_id']:
        raise Fail('This is a different version of the game (main build ID %s). This release supports %s (build ID %s).'
                   % (bid, want['version'], want['main_build_id']))
    names = list(want['sha256'])
    for i, n in enumerate(names):
        progress('verify', i, len(names), 'Checking your game files: %s' % n)
        got = sha256(os.path.join(exefs, n))
        if got != want['sha256'][n]:
            raise Fail('exefs/%s does not match the supported %s dump (sha256 %s). Re-dump the game, or check that the update %s is installed.'
                       % (n, want['version'], got, want['version']))
    progress('verify', len(names), len(names), 'Game files match %s' % want['version'])
    return want


def popen(cmd, **kw):
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                            creationflags=CREATE_NO_WINDOW | BELOW_NORMAL, **kw)


class Runner:
    """Runs tool processes; kills them all when the cancel file appears."""

    def __init__(self, cancel_file):
        self.cancel_file = cancel_file
        self.procs = set()
        self.lock = threading.Lock()
        self.cancelled = False
        threading.Thread(target=self._watch, daemon=True).start()

    def _watch(self):
        while True:
            if os.path.exists(self.cancel_file):
                self.cancelled = True
                with self.lock:
                    for p in list(self.procs):
                        try:
                            # the whole tree: `zig c++` runs its clang as a child process, which p.kill() would leave running
                            subprocess.run(['taskkill', '/F', '/T', '/PID', str(p.pid)], stdout=subprocess.DEVNULL,
                                           stderr=subprocess.DEVNULL, creationflags=CREATE_NO_WINDOW)
                            p.kill()
                        except OSError:
                            pass
            time.sleep(0.3)

    def check(self):
        if self.cancelled or os.path.exists(self.cancel_file):
            self.cancelled = True
            raise Cancelled()

    def run(self, cmd, env=None, cwd=None, on_line=None):
        self.check()
        p = popen(cmd, env=env, cwd=cwd)
        with self.lock:
            self.procs.add(p)
        tail = []
        for raw in p.stdout:
            line = raw.decode('utf-8', 'replace').rstrip()
            tail.append(line)
            del tail[:-30]
            if on_line:
                on_line(line)
        rc = p.wait()
        with self.lock:
            self.procs.discard(p)
        self.check()
        return rc, '\n'.join(tail)


def plain_exefs(runner, exefs, build):
    """The lifter reads plain NSOs: ZBIC-compressed ones (NSO flag bit 7) are converted into <build>/plain first."""
    zbic = []
    for name, nso, _, _, _, _ in MODULES:
        with open(os.path.join(exefs, nso), 'rb') as f:
            head = f.read(0x10)
        if int.from_bytes(head[12:16], 'little') & 0x80:
            zbic.append(nso)
    if not zbic:
        return exefs
    out = os.path.join(build, 'plain')
    if all(os.path.isfile(os.path.join(out, n)) for _, n, _, _, _, _ in MODULES) and os.path.isfile(os.path.join(out, '.done')):
        return out
    if not os.path.isfile(PLAIN_NSO):
        raise Fail('The bundled NSO converter is missing (%s). Extract the release zip again.' % PLAIN_NSO)
    progress('verify', 0, 1, 'Unpacking the compressed game code (%s)' % ', '.join(zbic))
    shutil.rmtree(out, ignore_errors=True)
    rc, tail = runner.run([PLAIN_NSO, exefs, out])
    if rc != 0:
        raise Fail('Unpacking the game code failed (exit %d):\n%s' % (rc, tail))
    open(os.path.join(out, '.done'), 'w').write('ok\n')
    return out


def copy_runtime(game):
    """The runtime DLL and what it loads next to the exe (DLL search order: the exe's folder first)."""
    if not RUNTIME:
        return
    for root, _, files in os.walk(RUNTIME):
        rel = os.path.relpath(root, RUNTIME)
        dst = game if rel == '.' else os.path.join(game, rel)
        os.makedirs(dst, exist_ok=True)
        for f in files:
            shutil.copy2(os.path.join(root, f), os.path.join(dst, f))


def boot_check(exefs, plain):
    """Before the long translation: stop on what keeps the game from starting at all (nxrecomp/bootcheck.py)."""
    progress('verify', 0, 1, 'Checking that the game can start')
    sys.path.insert(0, RECOMP)
    from nxrecomp import bootcheck
    errors, warnings = bootcheck.check(exefs, [])                    # the files the loader needs (main.npdm is not in plain)
    e2, w2 = bootcheck.check(plain, [(n, f, base) for n, f, base, _, _, _ in MODULES])
    errors += [e for e in e2 if 'is missing' not in e]
    for w in warnings + w2:
        print('boot check: warning: ' + w, flush=True)
    if errors:
        raise Fail('The game cannot start from this dump:\n' + '\n'.join(errors))
    progress('verify', 1, 1, 'The game can start (%d warning(s) in the log)' % len(warnings + w2))


def lift(runner, exefs, gen, jobs):
    """One lifter process per module (the lifter itself runs `jobs` worker processes)."""
    for k, (name, nso, base, index, shards, sites) in enumerate(MODULES):
        out = os.path.join(gen, name)
        marker = os.path.join(out, '.lifted')
        if os.path.isfile(marker):
            continue
        progress('lift', k, len(MODULES), 'Translating the game code: %s (%d of %d)' % (name, k + 1, len(MODULES)))
        if os.path.isdir(out):
            shutil.rmtree(out)
        os.makedirs(out)
        args = ['lift', os.path.join(exefs, nso), out, '--name', name, '--base', hex(base), '--shards', str(shards),
                '--jobs', str(jobs), '--game', 'generic' if GENERIC else GAME_ID, '--index', str(index)]
        if sites and not GENERIC:
            args += ['--sites', os.path.join(RECOMP, 'games', GAME_ID, sites)]
        cmd = [sys.executable, '-I', os.path.abspath(__file__), '--lift-one', json.dumps(args)]
        rc, tail = runner.run(cmd, cwd=RECOMP)
        if rc != 0:
            raise Fail('Translating %s failed (exit %d):\n%s' % (name, rc, tail))
        open(marker, 'w').write('ok\n')
    progress('lift', len(MODULES), len(MODULES), 'Game code translated')


def compile_all(runner, gen, obj, jobs, env):
    units = []
    for name, _, _, _, shards, _ in MODULES:
        d = os.path.join(gen, name)
        for f in sorted(os.listdir(d)):
            if f.endswith('.cpp'):
                units.append((name, os.path.join(d, f)))
    total = len(units)
    todo = []
    for name, src in units:
        o = os.path.join(obj, name, os.path.basename(src)[:-4] + '.o')
        if not os.path.isfile(o):
            todo.append((name, src, o))
    done = total - len(todo)
    progress('compile', done, total, 'Compiling: %d of %d' % (done, total))
    for name, _, _, _, _, _ in MODULES:
        os.makedirs(os.path.join(obj, name), exist_ok=True)
    # biggest first, so the long ones do not end up last on a single core
    todo.sort(key=lambda t: -os.path.getsize(t[1]))

    def one(t):
        name, src, o = t
        tmp = o + '.tmp'
        cmd = [ZIG, 'c++'] + CXXFLAGS + ['-I', os.path.join(RECOMP, 'include'), '-I', os.path.dirname(src), '-c', src, '-o', tmp]
        rc, tail = runner.run(cmd, env=env)
        if rc != 0:
            raise Fail('Compiling %s/%s failed (exit %d):\n%s' % (name, os.path.basename(src), rc, tail))
        os.replace(tmp, o)

    with ThreadPoolExecutor(max_workers=jobs) as ex:
        pending = {ex.submit(one, t) for t in todo}
        try:
            while pending:
                fin, pending = wait(pending, return_when=FIRST_COMPLETED)
                for f in fin:
                    f.result()
                    done += 1
                    progress('compile', done, total, 'Compiling: %d of %d' % (done, total))
        except BaseException:
            for f in pending:
                f.cancel()
            raise


def clear_high_entropy_va(exe):
    """The game exe keeps host allocations low (the guest ranges at 0x71_0000_0000 must stay free): no high-entropy ASLR,
    as /HIGHENTROPYVA:NO does for the clang-cl build. zig's linker has no flag for it, so clear the PE bit."""
    with open(exe, 'r+b') as f:
        f.seek(0x3c)
        pe = int.from_bytes(f.read(4), 'little')
        f.seek(pe + 24)
        if f.read(2) != b'\x0b\x02':
            raise Fail('%s is not a PE32+ image' % EXE_NAME)
        off = pe + 24 + 70
        f.seek(off)
        dc = int.from_bytes(f.read(2), 'little') & ~0x20
        f.seek(off)
        f.write(dc.to_bytes(2, 'little'))


def write_gen(build):
    """The files the stub and the resource script need, from the manifest: game_manifest.h (the macros of the generated header of the
    runtime build that this side uses), game.manifest (from sdk/game.manifest.in), game.rc (the staged template)."""
    gen = os.path.join(build, 'gen')
    os.makedirs(gen, exist_ok=True)
    g = MANIFEST['game']
    entry = [m for m in MANIFEST['module'] if m.get('entry')]
    order = entry + [m for m in MANIFEST['module'] if not m.get('entry')]
    lines = ['// Generated from the game manifest by game_build.py.', '#pragma once',
             '#define GAME_ID "%s"' % g['id'], '#define GAME_PRODUCT "%s"' % g['product'], '#define GAME_EXE "%s"' % EXE_NAME,
             '#define GAME_MODULES(X) \\']
    lines += ['    X(%s, "%s", 0x%xull)%s' % (m['name'], m['file'], m['base'], ' \\' if k + 1 < len(order) else '') for k, m in enumerate(order)]
    with open(os.path.join(gen, 'game_manifest.h'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    with open(os.path.join(SDK, 'game.manifest.in'), encoding='utf-8', newline='') as f:
        text = f.read()
    text = text.replace('@GAME_ID@', g['id']).replace('@GAME_PRODUCT@', g['product'])
    with open(os.path.join(gen, 'game.manifest'), 'w', encoding='utf-8', newline='') as f:
        f.write(text)
    shutil.copyfile(os.path.join(SDK, 'game.rc'), os.path.join(gen, 'game.rc'))
    return gen


def link(runner, obj, game, env, gen):
    progress('link', 0, 1, 'Linking %s' % EXE_NAME)
    libs = []
    for name, _, _, _, _, _ in MODULES:
        a = os.path.join(obj, '%s_%s.a' % (GAME_ID, name))
        if os.path.isfile(a):
            os.remove(a)
        objs = sorted(os.path.join(obj, name, f) for f in os.listdir(os.path.join(obj, name)) if f.endswith('.o'))
        rsp = os.path.join(obj, '%s.rsp' % name)
        with open(rsp, 'w') as f:
            f.write('\n'.join('"%s"' % p.replace('\\', '/') for p in objs))
        rc, tail = runner.run([ZIG, 'ar', 'rcs', a, '@' + rsp], env=env)
        if rc != 0:
            raise Fail('Archiving %s failed (exit %d):\n%s' % (name, rc, tail))
        libs.append(a)
    tmp = os.path.join(obj, EXE_NAME)
    cmd = [ZIG, 'c++', '-target', 'x86_64-windows-gnu', '-mcpu=x86_64+cx16', '-O1', '-fno-exceptions', '-fno-rtti', '-I', SDK, '-I', gen,
           os.path.join(SDK, 'stub.cpp'), os.path.join(gen, 'game.rc')] + libs + [os.path.join(SDK, RT_LIB),
           '-o', tmp, '-Wl,--stack,134217728', '-Wl,--subsystem,windows']
    rc, tail = runner.run(cmd, env=env, cwd=gen)
    if rc != 0:
        raise Fail('Linking %s failed (exit %d):\n%s' % (EXE_NAME, rc, tail))
    clear_high_entropy_va(tmp)
    dst = os.path.join(game, EXE_NAME)
    os.replace(tmp, dst)
    progress('link', 1, 1, '%s built' % EXE_NAME)
    return dst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exefs')
    ap.add_argument('--game')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument('--keep', action='store_true', help='keep the generated sources and objects')
    ap.add_argument('--manifest', default=os.path.join(HERE, '..', '..', 'game.toml'), help='the game manifest (default: game.toml in the package root)')
    ap.add_argument('--sdk', help='folder with stub.cpp, rt_api.h, game.rc, game.manifest.in and the runtime import library (default: sdk next to this script)')
    ap.add_argument('--runtime', help='folder whose files (runtime DLL, decoder DLLs, fonts) are copied next to the exe')
    ap.add_argument('--lift-one', help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.lift_one:                       # child process: one lifter run (keeps the lifter's own pool out of this process)
        sys.path.insert(0, RECOMP)
        from nxrecomp.__main__ import main as nx_main
        return nx_main(json.loads(a.lift_one)) or 0
    if not a.exefs or not a.game:
        ap.error('--exefs and --game are required')
    load_manifest(a.manifest)
    global SDK, RUNTIME
    if a.sdk:
        SDK = os.path.abspath(a.sdk)
    if a.runtime:
        RUNTIME = os.path.abspath(a.runtime)
    game = os.path.abspath(a.game)
    build = os.path.join(game, 'build')
    os.makedirs(build, exist_ok=True)
    cancel_file = os.path.join(build, 'cancel')
    if os.path.exists(cancel_file):
        os.remove(cancel_file)
    t0 = time.time()
    times = {}
    try:
        if not os.path.isfile(ZIG):
            raise Fail('The bundled compiler is missing (%s). Extract the release zip again.' % ZIG)
        want = verify(a.exefs)
        stamp = {'format': FORMAT, 'main_build_id': want['main_build_id'], 'sha256': want['sha256'],
                 'runtime': sha256(os.path.join(SDK, RT_LIB))}
        state_p = os.path.join(build, 'state.json')
        old = json.load(open(state_p)) if os.path.isfile(state_p) else None
        if old != stamp:                 # another dump or another release: start over
            for d in ('lift', 'obj', 'plain'):
                shutil.rmtree(os.path.join(build, d), ignore_errors=True)
            json.dump(stamp, open(state_p, 'w'), indent=1)
        times['verify'] = round(time.time() - t0, 1)
        runner = Runner(cancel_file)
        env = dict(os.environ)
        env['ZIG_GLOBAL_CACHE_DIR'] = os.path.join(build, 'zig-cache')
        env['ZIG_LOCAL_CACHE_DIR'] = os.path.join(build, 'zig-cache')
        gen, obj = os.path.join(build, 'lift'), os.path.join(build, 'obj')
        t = time.time()
        plain = plain_exefs(runner, a.exefs, build)
        boot_check(a.exefs, plain)
        lift(runner, plain, gen, a.jobs)
        times['lift'] = round(time.time() - t, 1)
        t = time.time()
        compile_all(runner, gen, obj, a.jobs, env)
        times['compile'] = round(time.time() - t, 1)
        t = time.time()
        exe = link(runner, obj, game, env, write_gen(build))
        copy_runtime(game)
        if GENERIC:          # the generic runtime takes the game's identity and title facts from it at start
            shutil.copyfile(MANIFEST_PATH, os.path.join(game, 'game.toml'))
        times['link'] = round(time.time() - t, 1)
        if not a.keep:
            progress('finish', 0, 1, 'Removing the build files')
            for d in ('lift', 'obj', 'zig-cache', 'gen', 'plain'):
                shutil.rmtree(os.path.join(build, d), ignore_errors=True)
        done = dict(stamp, exe_sha256=sha256(exe), seconds=times, finished=time.strftime('%Y-%m-%d %H:%M:%S'))
        json.dump(done, open(os.path.join(build, 'done.json'), 'w'), indent=1)
        progress('finish', 1, 1, '%s is ready (%d s)' % (EXE_NAME, time.time() - t0))
        return 0
    except Cancelled:
        say('cancelled', 'Build paused. Set up again continues where it stopped.')
        return 2
    except Fail as e:
        say('error', str(e).replace('\n', '\\n'))
        return 1
    except Exception as e:            # an unexpected failure: say what it was, plainly
        say('error', '%s: %s' % (type(e).__name__, str(e).replace('\n', '\\n')))
        return 1


if __name__ == '__main__':
    sys.exit(main())
