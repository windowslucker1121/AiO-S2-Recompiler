# SPDX-License-Identifier: GPL-3.0-or-later
"""make_manifest: writes an experimental game manifest (game.toml) for a title that has no port yet, from the player's own dump.

    python -I tools/extract/make_manifest.py <dump folder | exefs folder> --out <dir>/game.toml [--name "Game"] [--version N]
                                             [--display-version 1.2.3] [--nacp control.nacp] [--json]

Everything comes from the dump's ExeFS (decrypted, as nup_extract.py writes it):
  - the modules (rtld, main, subsdk0..9, sdk) with load addresses, lifter indices and translation-unit counts (text size),
  - sha256 of every ExeFS file and main's build id (the game build checks the dump against them),
  - main.npdm: program id (ACI0), main thread priority / core / stack, SystemResourceSize,
  - the platform: platform 2 when the SDK version string in sdk is 20 or newer or main is ZBIC-compressed, else platform 1,
  - control.nacp when one is given or found next to the ExeFS (title, display version); else placeholders.
The manifest says `generic = true`: it runs on the generic runtime (generic_rt.dll, runtime/games/generic), which reads the [title] table
at start. Nothing here hooks the game: an experimental build boots with what the runtime covers for every title.
The manifest holds facts of the player's own dump and stays on their PC: never commit one (LEGAL.md).
Exit codes: 0 written, 1 error. --json prints {"id", "title_id", "platform", "path"} on success.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys

# Load addresses: the layout every port uses (main 0x71.., sdk 0x75.., subsdk0 0x76.., rtld 0x77..); further subsdks after rtld.
BASES = {'main': 0x7100000000, 'sdk': 0x7500000000, 'subsdk0': 0x7600000000, 'rtld': 0x7700000000}
SUBSDK_BASE = 0x7800000000            # subsdk1..9: 1 GiB apart from here (up to 0x7a00000000, inside the 39-bit ASLR region)
INSNS_PER_UNIT = 45000                # lifter translation units: about this many instructions each


class Fail(Exception):
    pass


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def find_exefs(src):
    """The ExeFS of a dump: the folder itself, the newest updates/<n>/exefs, or base/exefs. Returns (exefs, update version or 0)."""
    src = os.path.abspath(src)
    if os.path.isfile(os.path.join(src, 'main.npdm')):
        tail = os.path.basename(os.path.dirname(src))
        return src, int(tail) if tail.isdigit() else 0
    if os.path.isfile(os.path.join(src, 'exefs', 'main.npdm')):
        return os.path.join(src, 'exefs'), 0
    best = None
    up = os.path.join(src, 'updates')
    if os.path.isdir(up):
        for n in os.listdir(up):
            e = os.path.join(up, n, 'exefs')
            if n.isdigit() and os.path.isfile(os.path.join(e, 'main.npdm')) and (best is None or int(n) > best[1]):
                best = (e, int(n))
    if best:
        return best
    if os.path.isfile(os.path.join(src, 'base', 'exefs', 'main.npdm')):
        return os.path.join(src, 'base', 'exefs'), 0
    raise Fail('no exefs/main.npdm in %s (also looked in updates/<version>/ and base/)' % src)


def read_npdm(path):
    d = open(path, 'rb').read()
    if len(d) < 0x80 or d[:4] != b'META':
        raise Fail('main.npdm is not an NPDM (META) file')
    prio, core = d[0x0e], d[0x0f]
    srs, = struct.unpack_from('<I', d, 0x14)
    stack, = struct.unpack_from('<I', d, 0x1c)
    name = d[0x20:0x30].split(b'\0')[0].decode('ascii', 'replace')
    aci, = struct.unpack_from('<I', d, 0x70)
    if aci + 0x18 > len(d) or d[aci:aci + 4] != b'ACI0':
        raise Fail('main.npdm has no ACI0 section')
    pid, = struct.unpack_from('<Q', d, aci + 0x10)
    return {'program_id': pid, 'priority': prio, 'core': core, 'system_resource_size': srs, 'stack': stack, 'name': name}


def nso_header(path):
    with open(path, 'rb') as f:
        h = f.read(0x100)
    if len(h) < 0x100 or h[:4] != b'NSO0':
        raise Fail('%s is not a decrypted NSO (NSO0). Extract the game with your own dump tools first.' % os.path.basename(path))
    flags, = struct.unpack_from('<I', h, 12)
    text_size, = struct.unpack_from('<I', h, 0x18)
    return {'flags': flags, 'text_size': text_size, 'build_id': h[0x40:0x54].hex(), 'zbic': bool(flags & 0x80)}


def sdk_version(path):
    """(major, text) of the NintendoSdk_nnSdk string in a module, or (None, '') when it cannot be read."""
    try:
        d = open(path, 'rb').read()
        flags, = struct.unpack_from('<I', d, 12)
        if flags & 0x80:
            return None, ''
        blob = b''
        for i, (ho, cso) in enumerate(((0x10, 0x60), (0x20, 0x64), (0x30, 0x68))):
            foff, _, size = struct.unpack_from('<III', d, ho)
            csz, = struct.unpack_from('<I', d, cso)
            raw = d[foff:foff + csz]
            if flags & (1 << i):
                import lz4.block
                raw = lz4.block.decompress(raw, uncompressed_size=size)
            blob += raw
    except Exception:
        return None, ''
    m = re.search(rb'NintendoSdk_nnSdk-(?:[A-Za-z]+_)?(\d+)_(\d+)_(\d+)', blob)
    if not m:
        return None, ''
    return int(m.group(1)), '%s.%s.%s' % (m.group(1).decode(), m.group(2).decode(), m.group(3).decode())


def read_nacp(path):
    """Title (American English entry, else the first non-empty one) and display version of a control.nacp."""
    d = open(path, 'rb').read()
    if len(d) < 0x4000:
        raise Fail('%s is too small for a control.nacp' % path)
    title = ''
    for i in range(16):
        t = d[i * 0x300:i * 0x300 + 0x200].split(b'\0')[0].decode('utf-8', 'replace').strip()
        if t:
            title = t
            break
    disp = d[0x3060:0x3070].split(b'\0')[0].decode('ascii', 'replace').strip()
    langs, = struct.unpack_from('<I', d, 0x302c)
    return {'title': title, 'display_version': disp, 'languages': langs}


def name_from_path(src):
    n = os.path.basename(os.path.normpath(src))
    if n.lower().endswith('.nup'):
        n = n[:-4]
    n = re.sub(r'[\[(][^\])]*[\])]', '', n).replace('_', ' ')
    return ' '.join(n.split())


def toml_str(s):
    # GameManifest.cmake reads no quotes or backslashes inside strings
    return '"%s"' % ''.join(c for c in str(s) if c not in '"\\\n\r').strip()


def make(src, name=None, version=None, display_version=None, nacp=None):
    exefs, upd = find_exefs(src)
    if version is None:
        version = upd
    npdm = read_npdm(os.path.join(exefs, 'main.npdm'))
    tid = '%016X' % npdm['program_id']
    present = [n for n in os.listdir(exefs) if os.path.isfile(os.path.join(exefs, n))]
    for need in ('main', 'rtld', 'sdk'):
        if need not in present:
            raise Fail('exefs/%s is missing: pick the ExeFS of a complete dump' % need)
    names = ['main', 'sdk'] + ['subsdk%d' % i for i in range(10) if 'subsdk%d' % i in present] + ['rtld']
    heads = {n: nso_header(os.path.join(exefs, n)) for n in names}
    major, sdk_text = sdk_version(os.path.join(exefs, 'sdk'))
    switch2 = (major is not None and major >= 20) or heads['main']['zbic']
    info = {}
    if not nacp:
        for cand in (os.path.join(os.path.dirname(exefs), 'control.nacp'), os.path.join(exefs, 'control.nacp')):
            if os.path.isfile(cand):
                nacp = cand
                break
    if nacp:
        info = read_nacp(nacp)
    title = info.get('title') or name or (name_from_path(src) if not os.path.isfile(os.path.join(src, 'main.npdm')) else '') \
        or npdm['name'] or tid
    vtext = '%d.%d.%d' % (version >> 16, (version >> 8) & 255, version & 255)
    disp = display_version or info.get('display_version') or vtext
    gid = 't' + tid.lower()
    langs = info.get('languages') or 0x1ffff
    firmware = '%d.0.0' % major if major else ('22.0.0' if switch2 else '16.0.0')

    L = []
    w = L.append
    w('# Experimental game manifest written by make_manifest.py from the player\'s own dump (never commit this file: LEGAL.md).')
    w('# It runs on the generic runtime (runtime/games/generic): no game-specific hooks, fixes or input mapping.')
    w('schema = 1')
    w('')
    w('[game]')
    w('id = %s' % toml_str(gid))
    w('title = %s' % toml_str(title))
    w('short_title = %s' % toml_str(title))
    w('product = %s' % toml_str('Recomp module: ' + gid))
    w('window_title = %s' % toml_str(title + ' (unofficial recompilation, experimental)'))
    w('title_id = %s' % toml_str(tid))
    w('version = %s' % toml_str(vtext))
    w('platform = %s' % toml_str('switch2' if switch2 else 'switch1'))
    w('languages = 0x%x' % langs)
    w('generic = true')
    w('')
    w('[files]')
    w('exe = %s' % toml_str(gid + '.exe'))
    w('runtime_dll = "generic_rt.dll"')
    w('ini = %s' % toml_str(gid + '.ini'))
    w('log = %s' % toml_str(gid + '.log'))
    w('save_folder = %s' % toml_str(gid + '-recomp'))
    w('')
    w('[dump]')
    w('version = %s' % toml_str('%s (update %d)' % (vtext, version) if version else vtext))
    w('display_version = %s' % toml_str(disp))
    w('main_build_id = %s' % toml_str(heads['main']['build_id']))
    for f in sorted(present):
        w('[[dump.file]]')
        w('name = %s' % toml_str(f))
        w('sha256 = %s' % toml_str(sha256(os.path.join(exefs, f))))
    w('')
    w('# Title facts the generic runtime starts the game with (main.npdm; save sizes are placeholders without a control.nacp).')
    w('[title]')
    w('main_thread_priority = %d' % npdm['priority'])
    w('main_thread_core = %d' % npdm['core'])
    w('main_thread_stack = 0x%x' % npdm['stack'])
    w('system_resource_size = 0x%x' % npdm['system_resource_size'])
    w('firmware = %s' % toml_str(firmware))
    w('sdk = %s' % toml_str(sdk_text or 'unknown'))
    w('save_size = 0x4000000')
    w('temporary_storage_size = 0x4000000')
    w('')
    index = {'main': 0, 'sdk': 1}
    k = 2
    for n in names:
        if n.startswith('subsdk'):
            index[n] = k
            k += 1
    index['rtld'] = k
    for n in names:
        if n in BASES:
            base = BASES[n]
        else:
            base = SUBSDK_BASE + (int(n[6:]) - 1) * 0x40000000
        units = max(1, -(-heads[n]['text_size'] // 4 // INSNS_PER_UNIT))
        w('[[module]]')
        w('name = %s' % toml_str(n))
        w('file = %s' % toml_str(n))
        w('base = 0x%x' % base)
        w('index = %d' % index[n])
        w('units = %d' % units)
        if n == 'rtld':
            w('entry = true')
    w('')
    w('[shaders]')
    w('extractor = "generic"')
    w('extractor_script = "tools/shaders/scripts/extract_generic.py"')
    return '\n'.join(L) + '\n', {'id': gid, 'title_id': tid, 'platform': 'switch2' if switch2 else 'switch1', 'title': title,
                                  'sdk': sdk_text, 'zbic': heads['main']['zbic']}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dump', help='a dump folder (base/, updates/<n>/) or an ExeFS folder')
    ap.add_argument('--out', required=True, help='the game.toml to write')
    ap.add_argument('--name', help='the game\'s title (default: control.nacp, else the dump\'s file name)')
    ap.add_argument('--version', type=int, help='the update version number (default: the updates/<n> folder name)')
    ap.add_argument('--display-version')
    ap.add_argument('--nacp', help='a control.nacp of the game')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    try:
        text, facts = make(a.dump, a.name, a.version, a.display_version, a.nacp)
    except (Fail, OSError) as e:
        print('error: %s' % e, file=sys.stderr)
        return 1
    out = os.path.abspath(a.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    tmp = out + '.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    os.replace(tmp, out)
    if a.json:
        print(json.dumps(dict(facts, path=out)))
    else:
        print('%s: %s (%s, %s) -> %s' % (facts['id'], facts['title'], facts['title_id'], facts['platform'], out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
