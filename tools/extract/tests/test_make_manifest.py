# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks make_manifest on synthetic dumps (made-up NSO headers and NPDM; no game data): modules, bases, indices, units, npdm facts,
the platform rule (SDK 20+ or ZBIC = Switch 2), hashes, and that the result reads as TOML with what game_build.py needs.
  python tools/extract/tests/test_make_manifest.py
"""
import hashlib
import os
import struct
import sys
import tempfile
import tomllib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import make_manifest                            # noqa: E402


def nso(text_size, extra=b'', zbic=False, build_id=b'\x11' * 20):
    """An NSO with three stored (uncompressed) segments: text of text_size zero bytes, rodata = extra, empty data."""
    text = bytes(text_size)
    segs = [text, extra, b'']
    h = bytearray(0x100)
    h[0:4] = b'NSO0'
    struct.pack_into('<I', h, 12, 0x80 if zbic else 0)
    off, moff = 0x100, 0
    for i, s in enumerate(segs):
        struct.pack_into('<III', h, 0x10 + i * 0x10, off, moff, len(s))
        struct.pack_into('<I', h, 0x60 + i * 4, len(s))
        h[0xA0 + i * 0x20:0xC0 + i * 0x20] = hashlib.sha256(s).digest()
        off += len(s)
        moff += (len(s) + 0xfff) & ~0xfff
    h[0x40:0x54] = build_id
    return bytes(h) + b''.join(segs)


def npdm(pid, prio=49, core=0, stack=0x80000, srs=0x200000, name=b'Application'):
    d = bytearray(0x100)
    d[0:4] = b'META'
    d[0x0e] = prio
    d[0x0f] = core
    struct.pack_into('<I', d, 0x14, srs)
    struct.pack_into('<I', d, 0x1c, stack)
    d[0x20:0x20 + len(name)] = name
    struct.pack_into('<I', d, 0x70, 0x80)
    d[0x80:0x84] = b'ACI0'
    struct.pack_into('<Q', d, 0x90, pid)
    return bytes(d)


def dump(root, sdk_text, zbic=False, subsdks=(0,), version=131072):
    e = os.path.join(root, 'updates', str(version), 'exefs')
    os.makedirs(e)
    files = {'main': nso(45000 * 4 * 3 + 4, zbic=zbic, build_id=bytes(range(20))), 'rtld': nso(0x4000),
             'sdk': nso(45000 * 4 + 8, extra=b'\0SDK MW+Nintendo+NintendoSdk_nnSdk-' + sdk_text + b'-Release\0'),
             'main.npdm': npdm(0x0123456789ABC000)}
    for i in subsdks:
        files['subsdk%d' % i] = nso(0x1000)
    for n, b in files.items():
        with open(os.path.join(e, n), 'wb') as f:
            f.write(b)
    return e


def main():
    with tempfile.TemporaryDirectory() as t:
        # Switch 2 by its SDK version; a subsdk past subsdk0 goes after rtld
        dump(os.path.join(t, 'a'), b'Generic_22_2_8', subsdks=(0, 1))
        text, facts = make_manifest.make(os.path.join(t, 'a'), name='Test Game')
        m = tomllib.loads(text)
        assert facts['platform'] == 'switch2' and facts['sdk'] == '22.2.8', facts
        g = m['game']
        assert g['id'] == 't0123456789abc000' and g['title_id'] == '0123456789ABC000' and g['generic'] is True, g
        assert g['version'] == '2.0.0' and g['title'] == 'Test Game', g
        assert m['files']['runtime_dll'] == 'generic_rt.dll' and m['files']['exe'] == 't0123456789abc000.exe'
        assert m['dump']['main_build_id'] == bytes(range(20)).hex()
        mods = {x['name']: x for x in m['module']}
        assert [x['name'] for x in m['module']] == ['main', 'sdk', 'subsdk0', 'subsdk1', 'rtld']
        assert mods['main']['base'] == 0x7100000000 and mods['main']['units'] == 4 and mods['main']['index'] == 0
        assert mods['sdk']['base'] == 0x7500000000 and mods['sdk']['units'] == 2 and mods['sdk']['index'] == 1
        assert mods['subsdk0']['base'] == 0x7600000000 and mods['subsdk1']['base'] == 0x7800000000
        assert mods['rtld']['base'] == 0x7700000000 and mods['rtld'].get('entry') is True and mods['rtld']['index'] == 4
        assert len({x['base'] for x in m['module']}) == 5 and len({x['index'] for x in m['module']}) == 5
        ti = m['title']
        assert (ti['main_thread_priority'], ti['main_thread_core'], ti['main_thread_stack'], ti['system_resource_size']) == (49, 0, 0x80000, 0x200000)
        assert ti['firmware'] == '22.0.0'
        files = {f['name']: f['sha256'] for f in m['dump']['file']}
        e = os.path.join(t, 'a', 'updates', '131072', 'exefs')
        assert set(files) == {'main', 'rtld', 'sdk', 'subsdk0', 'subsdk1', 'main.npdm'}
        assert files['sdk'] == hashlib.sha256(open(os.path.join(e, 'sdk'), 'rb').read()).hexdigest()

        # Switch 1 by its SDK version, the ExeFS folder itself as input
        e = dump(os.path.join(t, 'b'), b'16_2_12', subsdks=())
        text, facts = make_manifest.make(e)
        m = tomllib.loads(text)
        assert facts['platform'] == 'switch1' and m['game']['platform'] == 'switch1' and m['title']['firmware'] == '16.0.0', facts
        assert [x['name'] for x in m['module']] == ['main', 'sdk', 'rtld']

        # ZBIC main = Switch 2 even without a readable SDK string
        dump(os.path.join(t, 'c'), b'x', zbic=True, subsdks=())
        assert make_manifest.make(os.path.join(t, 'c'))[1]['platform'] == 'switch2'

        # the command line writes the file; a dump without sdk is refused
        out = os.path.join(t, 'out', 'game.toml')
        assert make_manifest.main([os.path.join(t, 'a'), '--out', out]) == 0 and os.path.isfile(out)
        os.remove(os.path.join(t, 'c', 'updates', '131072', 'exefs', 'sdk'))
        assert make_manifest.main([os.path.join(t, 'c'), '--out', out]) == 1
    print('make_manifest: all checks passed')


if __name__ == '__main__':
    main()
