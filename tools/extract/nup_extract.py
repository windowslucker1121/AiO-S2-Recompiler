# SPDX-License-Identifier: GPL-3.0-or-later
"""nup_extract: turns a NUP package (.nup: a decrypted platform 1/2 dump, base game plus updates and DLC in one file; format:
https://github.com/a55uka/nup/blob/meow/SPEC.md) into the extracted layout the lifter, the runtime and the library app read:
  <out>/base/exefs/                 the base game's ExeFS (always)
  <out>/updates/<version>/exefs/    the newest update's ExeFS
  <out>/updates/<version>/romfs/    the newest update's RomFS over the base's (the whole game), or <out>/base/romfs/ without one
  <out>/extract.json                written last; its presence means complete (not written with --exefs-only)
NUP content is plaintext, so there are no keys. Every file is checked against its SHA-256 in the TOC; a second run keeps files whose
size already matches. ZBIC NSOs are written as stored (convert with nx_plain_nso, tools/extract).

  python tools/extract/nup_extract.py game.nup D:\\Game\\dump [--exefs-only]
  python tools/extract/nup_extract.py game.nup --list
"""
import argparse
import hashlib
import json
import os
import struct
import sys

NONE = 0xFFFFFFFF
KIND = {0: 'base', 1: 'update', 2: 'addon'}


class BadNup(Exception):
    pass


def zstd_decompress(b):
    try:
        from compression import zstd            # Python 3.14+
        return zstd.decompress(b)
    except ImportError:
        import zstandard
        return zstandard.ZstdDecompressor().decompress(b)


class Nup:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.size = os.fstat(self.f.fileno()).st_size
        sb = self.f.read(0x80)
        if len(sb) < 0x80 or sb[:4] != b'NUP\x01':
            raise BadNup('not a NUP v1 file')
        hs, _, toc_off, toc_size = struct.unpack_from('<HHQQ', sb, 4)
        self.blob_off, file_size, npk, flags = struct.unpack_from('<QQII', sb, 0x38)
        if file_size != self.size:
            raise BadNup(f'incomplete file: {self.size:,} of {file_size:,} bytes (copy or download it again)')
        if (hs < 0x80 or toc_off != hs or self.blob_off % 16 or self.blob_off < toc_off + toc_size or self.blob_off > self.size
                or flags or not npk):
            raise BadNup('bad superblock')
        self.f.seek(toc_off)
        self.toc = self.f.read(toc_size)
        if hashlib.sha256(self.toc).digest() != sb[0x18:0x38]:
            raise BadNup('TOC hash mismatch')
        magic, pc, self.ndir, self.nfile, self.sp_off, self.sp_size = struct.unpack_from('<4sIQQQQ', self.toc, 0)
        if magic != b'TOC\x01' or pc != npk or self.sp_off + self.sp_size > toc_size:
            raise BadNup('bad TOC')
        self.dir_at = 0x30 + 0x40 * pc
        self.file_at = self.dir_at + 0x18 * self.ndir
        self.ch_at = self.file_at + 0x50 * self.nfile
        self.packages = []
        for k in range(pc):
            tid, root, exefs, romfs, ver, kind, _, _, _, moff, msize = struct.unpack_from('<QIIIIBBHIQI', self.toc, 0x30 + 0x40 * k)
            if kind >= 5:
                raise BadNup(f'unknown package kind {kind}')
            self.packages.append(dict(title_id=f'{tid:016X}', exefs=exefs, romfs=romfs, version=ver, kind=kind, meta=(moff, msize)))

    def name(self, rec):
        off, n = struct.unpack_from('<II', self.toc, rec)
        if off + n > self.sp_size:
            raise BadNup('name outside the string pool')
        s = self.toc[self.sp_off + off:self.sp_off + off + n].decode()
        # names are labels, never paths (spec 6.1.5)
        if s in ('', '.', '..') or any(c in s for c in '/\\:') or any(ord(c) < 32 for c in s):
            raise BadNup(f'bad file name {s!r}')
        return s

    def tree(self, d, prefix=''):
        """{relative path: file record offset} under directory d."""
        out = {}
        if d == NONE:
            return out
        if d >= self.ndir:
            raise BadNup('bad directory index')
        first, count = struct.unpack_from('<II', self.toc, self.dir_at + 0x18 * d + 8)
        for i in range(first, first + count):
            n = struct.unpack_from('<I', self.toc, self.ch_at + 4 * i)[0]
            if n < self.ndir:
                out.update(self.tree(n, prefix + self.name(self.dir_at + 0x18 * n) + '/'))
            elif n - self.ndir < self.nfile:
                r = self.file_at + 0x50 * (n - self.ndir)
                out[prefix + self.name(r)] = r
            else:
                raise BadNup('bad node id')
        return out

    def meta(self, p, typ):
        off, size = p['meta']
        if not off or off + size > self.size:
            return None
        self.f.seek(off)
        m = self.f.read(size)
        i = 0
        while i + 8 <= size:
            t, _, n = struct.unpack_from('<HHI', m, i)
            if i + 8 + n > size:
                break
            if t == typ:
                return m[i + 8:i + 8 + n]
            i += 8 + n
        return None

    def write(self, r, dst):
        size, eo, es = struct.unpack_from('<QQQ', self.toc, r + 8)
        sha, codec = self.toc[r + 0x20:r + 0x40], self.toc[r + 0x40]
        if os.path.isfile(dst) and os.path.getsize(dst) == size:
            return size                         # kept from an earlier run
        if es and (eo < self.blob_off or eo + es > self.size):
            raise BadNup(f'{dst}: extent outside the blob region')
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        h = hashlib.sha256()
        tmp = dst + '.part'
        self.f.seek(eo)
        with open(tmp, 'wb') as o:
            if codec == 0:
                if es != size:
                    raise BadNup(f'{dst}: raw extent size differs from file size')
                left = size
                while left:
                    b = self.f.read(min(left, 1 << 22))
                    if not b:
                        raise BadNup(f'{dst}: short read')
                    h.update(b)
                    o.write(b)
                    left -= len(b)
            elif codec == 1:
                b = zstd_decompress(self.f.read(es))   # ponytail: whole file in memory, stream it if one outgrows RAM
                if len(b) != size:
                    raise BadNup(f'{dst}: zstd size mismatch')
                h.update(b)
                o.write(b)
            else:
                raise BadNup(f'{dst}: unknown codec {codec}')
        if h.digest() != sha:
            os.remove(tmp)
            raise BadNup(f'{dst}: SHA-256 mismatch')
        os.replace(tmp, dst)
        return size

    def write_tree(self, files, out, label):
        total = 0
        for i, (rel, r) in enumerate(sorted(files.items()), 1):
            total += self.write(r, os.path.join(out, *rel.split('/')))
            if i % 2000 == 0 or i == len(files):
                print(f'@progress {label} {i} {len(files)}', flush=True)
        return total


def extract(path, out, exefs_only=False):
    n = Nup(path)
    bases = [p for p in n.packages if p['kind'] == 0]
    updates = sorted((p for p in n.packages if p['kind'] == 1), key=lambda p: p['version'])
    if not bases:
        raise BadNup('no base game in this package (an update-only NUP needs its base)')
    base, upd = bases[0], (updates[-1] if updates else None)
    if base['exefs'] == NONE:
        raise BadNup('the base game has no ExeFS')
    n.write_tree(n.tree(base['exefs']), os.path.join(out, 'base', 'exefs'), 'exefs')
    rel = f'updates/{upd["version"]}' if upd else 'base'
    top = upd or base
    exefs = n.tree(top['exefs'])
    if upd:
        n.write_tree(exefs, os.path.join(out, *rel.split('/'), 'exefs'), 'exefs')
    if exefs_only:
        return None
    rom = n.tree(base['romfs'])
    if upd:
        rom.update(n.tree(upd['romfs']))        # the update's files shadow the base's (spec appendix A)
    nbytes = n.write_tree(rom, os.path.join(out, *rel.split('/'), 'romfs'), 'romfs')

    exe_dir = os.path.join(out, *rel.split('/'), 'exefs')
    with open(os.path.join(exe_dir, 'main'), 'rb') as f:
        build_id = f.read(0x60)[0x40:0x54].hex()
    zbic = 0
    for m in exefs:
        with open(os.path.join(exe_dir, m), 'rb') as f:
            h = f.read(16)
        zbic += len(h) == 16 and h[:4] == b'NSO0' and bool(struct.unpack_from('<I', h, 12)[0] & 0x80)
    result = dict(format=1, title_id=base['title_id'], dir=rel, version=upd['version'] if upd else 0,
                  display_version=(n.meta(top, 4) or n.meta(base, 4) or b'').decode(errors='replace'), build_id=build_id,
                  files=len(rom), bytes=nbytes, zbic_modules=zbic, inputs=[dict(path=path, size=n.size)])
    tmp = os.path.join(out, 'extract.json.part')
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(result, f, indent=2)
    os.replace(tmp, os.path.join(out, 'extract.json'))
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('nup')
    ap.add_argument('out', nargs='?')
    ap.add_argument('--exefs-only', action='store_true', help='write the ExeFS folders only (what the lifter needs)')
    ap.add_argument('--list', action='store_true', help='show the packages and write nothing')
    a = ap.parse_args()
    try:
        if a.list:
            n = Nup(a.nup)
            for p in n.packages:
                v = p['version']
                print(f'{KIND.get(p["kind"], p["kind"]):7} {p["title_id"]} v{v} ({v >> 16}.{(v >> 8) & 255}.{v & 255})  '
                      f'exefs={len(n.tree(p["exefs"]))} romfs={len(n.tree(p["romfs"]))}')
            return 0
        if not a.out:
            ap.error('out is required unless --list')
        r = extract(a.nup, a.out, a.exefs_only)
        print('@result ' + json.dumps(r) if r else '@result exefs written', flush=True)
        return 0
    except BadNup as e:
        print(f'@error {e}', flush=True)
        return 2
    except OSError as e:
        print(f'@error {e}', flush=True)
        return 4


if __name__ == '__main__':
    sys.exit(main())
