#!/usr/bin/env python3
"""A small .nup package for testing the Initial Setup end to end (runtime/apps/setup): the real ExeFS of an extracted dump (base and
update) and a few small shader archives of its romfs, so every step (extract, shader scan, translation, program list, preparation) runs
in minutes instead of hours. The game does not play from it (the romfs is a sliver of the real one).

    python -I make_test_nup.py --game <dump>/updates/<version> --base <dump>/base --out <file>.nup [--romfs-file Shader/X ...]

Built with tools/extract/tests/test_nup_extract.py's writer. Never commit its output (it holds game files).
"""
import argparse, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'extract', 'tests'))
import test_nup_extract as t   # noqa: E402

DEFAULT_FILES = [
    'Pack/ZsDic.pack.zs',                                                # zstd dictionaries the shader scan needs
    'Layout2Shader/MergeShader.Product.100.Nin_Ounce_NVN2.bfsha.zs',     # the UI shaders
    'Shader/SceneMaterial.Product.110.product.Nin_Ounce_NVN2.bfsha.zs',
    'Shader/TerrainGround.Product.110.product.Nin_Ounce_NVN2.bfsha.zs',
]


def tree_of(folder):
    return {n: open(os.path.join(folder, n), 'rb').read() for n in sorted(os.listdir(folder)) if os.path.isfile(os.path.join(folder, n))}


def put(tree, rel, data):
    parts = rel.split('/')
    for p in parts[:-1]:
        tree = tree.setdefault(p, {})
    tree[parts[-1]] = data


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', required=True, help='the update folder (exefs, romfs)')
    ap.add_argument('--base', required=True, help='the base folder (exefs)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--romfs-file', action='append', default=[])
    ap.add_argument('--title-id', default='0400486024DEC000')
    ap.add_argument('--version', type=int, default=131072)
    a = ap.parse_args()
    romfs = {}
    for rel in a.romfs_file or DEFAULT_FILES:
        put(romfs, rel, open(os.path.join(a.game, 'romfs', *rel.split('/')), 'rb').read())
    tid = int(a.title_id, 16)
    data = t.build([
        (tid, 0, 0, {'exefs': tree_of(os.path.join(a.base, 'exefs')), 'romfs': {'base_only.txt': b'base'}}),
        (tid | 0x800, 1, a.version, {'exefs': tree_of(os.path.join(a.game, 'exefs')), 'romfs': romfs}),
    ])
    with open(a.out, 'wb') as f:
        f.write(data)
    print(f'{a.out}: {len(data):,} bytes')


if __name__ == '__main__':
    main()
