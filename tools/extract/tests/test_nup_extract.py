# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks nup_extract on synthetic NUP files: update over base, ExeFS, extract.json, hash, name and truncation checks.
  python tools/extract/tests/test_nup_extract.py
"""
import hashlib
import json
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import nup_extract                              # noqa: E402

try:
    from compression import zstd                # Python 3.14+: one file goes in as codec 1
except ImportError:
    zstd = None


def build(pkgs):
    """pkgs: [(title_id, kind, version, tree)], tree = {name: bytes | tree}. Returns the bytes of a NUP v1 file."""
    dirs, files, pool, pk = [], [], bytearray(), []

    def s(name):
        b = name.encode()
        pool.extend(b)
        return len(pool) - len(b), len(b)

    def add_dir(name, tree):
        i = len(dirs)
        dirs.append(None)
        kids, sub = [], {}
        for k in sorted(tree):
            if isinstance(tree[k], dict):
                sub[k] = add_dir(k, tree[k])[0]
                kids.append(('d', sub[k]))
            else:
                files.append((s(k), tree[k], zstd is not None and k == 'c.bin'))
                kids.append(('f', len(files) - 1))
        dirs[i] = (s(name), kids)
        return i, sub

    for tid, kind, ver, tree in pkgs:
        root, sub = add_dir('', tree)
        pk.append((tid, root, sub.get('exefs', 0xFFFFFFFF), sub.get('romfs', 0xFFFFFFFF), ver, kind))
    nd, nf = len(dirs), len(files)
    children, ranges = [], []
    for _, kids in dirs:
        ranges.append((len(children), len(kids)))
        children += [k if t == 'd' else nd + k for t, k in kids]
    toc_size = 0x30 + 0x40 * len(pk) + 0x18 * nd + 0x50 * nf + 4 * len(children) + len(pool)
    blob_off = (0x80 + toc_size + 15) & ~15
    blob, recs = bytearray(), []
    for (no, nl), data, z in files:
        stored = zstd.compress(data) if z else data
        off = blob_off + len(blob)
        blob += stored + b'\0' * (-len(stored) % 16)
        recs.append(struct.pack('<IIQQQ32sB15x', no, nl, len(data), off if data else 0, len(stored), hashlib.sha256(data).digest(), int(z)))
    toc = struct.pack('<4sIQQQQQ', b'TOC\x01', len(pk), nd, nf, toc_size - len(pool), len(pool), 0)
    toc += b''.join(struct.pack('<QIIIIBB2x4xQI20x', *p, 0, 0, 0) for p in pk)
    toc += b''.join(struct.pack('<IIII8x', *d[0], *r) for d, r in zip(dirs, ranges))
    toc += b''.join(recs) + struct.pack(f'<{len(children)}I', *children) + bytes(pool)
    size = blob_off + len(blob)
    sb = struct.pack('<4sHHQQ32sQQII16s32x', b'NUP\x01', 0x80, 1, 0x80, len(toc), hashlib.sha256(toc).digest(), blob_off, size,
                     len(pk), 0, b'\0' * 16)
    return sb + toc + b'\0' * (blob_off - 0x80 - len(toc)) + blob


def nso(build_id):
    return b'NSO0' + bytes(0x3C) + build_id + bytes(0x100)


def expect_bad(data, what, tmp):
    p = os.path.join(tmp, 'bad.nup')
    with open(p, 'wb') as f:
        f.write(data)
    try:
        nup_extract.extract(p, os.path.join(tmp, 'bad'))
    except nup_extract.BadNup as e:
        assert what in str(e), e
    else:
        raise AssertionError(f'accepted a file with {what}')


def main():
    base_main, upd_main = nso(b'\x11' * 20), nso(b'\x22' * 20)
    good = build([
        (0x0400486024DEC000, 0, 0x10000, {'exefs': {'main': base_main, 'rtld': b'r'},
                                          'romfs': {'a.bin': b'base-a', 'sub': {'b.bin': b'base-b'}, 'empty.bin': b''}}),
        (0x0400486024DEC800, 1, 0x20000, {'exefs': {'main': upd_main, 'rtld': b'r'},
                                          'romfs': {'a.bin': b'upd-a', 'c.bin': b'new' * 100}}),
    ])
    with tempfile.TemporaryDirectory() as tmp:
        p, out = os.path.join(tmp, 'g.nup'), os.path.join(tmp, 'out')
        with open(p, 'wb') as f:
            f.write(good)
        r = nup_extract.extract(p, out)
        rd = lambda *a: open(os.path.join(out, *a), 'rb').read()   # noqa: E731
        assert rd('base', 'exefs', 'main') == base_main
        assert rd('updates', '131072', 'exefs', 'main') == upd_main
        assert rd('updates', '131072', 'romfs', 'a.bin') == b'upd-a'           # update shadows base
        assert rd('updates', '131072', 'romfs', 'sub', 'b.bin') == b'base-b'   # base-only file kept
        assert rd('updates', '131072', 'romfs', 'c.bin') == b'new' * 100
        assert rd('updates', '131072', 'romfs', 'empty.bin') == b''
        assert not os.path.exists(os.path.join(out, 'base', 'romfs'))
        j = json.load(open(os.path.join(out, 'extract.json')))
        assert j == r and j['dir'] == 'updates/131072' and j['build_id'] == '22' * 20 and j['files'] == 4, j
        assert nup_extract.extract(p, out) == r                                # second run keeps the files

        corrupt = bytearray(good)
        corrupt[good.index(b'upd-a')] ^= 1
        expect_bad(bytes(corrupt), 'SHA-256 mismatch', tmp)
        expect_bad(build([(1, 0, 0, {'exefs': {'main': b'x'}, 'romfs': {'..': b'x'}})]), 'bad file name', tmp)
        expect_bad(build([(1, 0, 0, {'exefs': {'main': b'x'}, 'romfs': {'a:b': b'x'}})]), 'bad file name', tmp)
        expect_bad(good[:-16], 'incomplete file', tmp)
        bad_toc = bytearray(good)
        bad_toc[0x80 + 0x30 + 0x40 * 2] ^= 1                                    # first dir record: TOC hash no longer matches
        expect_bad(bytes(bad_toc), 'TOC hash mismatch', tmp)
    print('nup_extract: all checks pass' + ('' if zstd else ' (zstd codec not covered: needs Python 3.14+)'))


if __name__ == '__main__':
    main()
