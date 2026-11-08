#!/usr/bin/env python
"""Build docs/mpr/patches.json (+ frag/web_patches.json symbol fragment) from community patch sources the developer keeps locally.

No patch bytes or cheat codes are part of this repository. The script reads them from two git-ignored files under work/kb/web/:

  fl4sh_patches.json      (optional) sets transcribed from a mod repository's pages:
                          {"source": "<repo url>", "commit": "<sha>",
                           "files": [{"dir": "<set folder>", "title_line": "...", "url": "...", "header_comments": ["..."],
                                      "entries": [{"offset": "0x...", "bytes": "<hex>", "comment": "..."}]}]}
  community_patches.json  (required) every other set, the facts and the build id:
                          {"format": 1,
                           "meta": {"build_id": "<main build id, hex>", "offset_note": "...", ...},
                           "sets": [{"id": "<source>/<name>", "title": "...", "author": "...", "repo": "<url>", "commit": "<sha>",
                                     "url": "<page the set was transcribed from>", "features": ["resolution", "fps", ...],
                                     "header_comments": ["..."],
                                     "entries": [{"offset": "0x<main-relative>", "bytes": "<hex, memory order>" | null,
                                                  "orig_bytes": "<hex>" (optional), "comment": "..."}]}],
                           "facts": [{"what": "...", "detail": "...", "status": "..."}]}

Offsets are main-relative (pchtxt offset_shift 0x100 = NSO header). "bytes" are in memory byte order: an Atmosphere cheat's numeric
32-bit word is written little-endian (word 0x52800020 -> "20008052").
"""
import json, re, pathlib, sys, collections
ROOT = pathlib.Path(__file__).resolve().parents[2]
W = ROOT / 'work/kb/web'
DATA = W / 'community_patches.json'

if not DATA.exists():
    sys.exit(f'{DATA} not found: the patch data is not part of the repository (format in this script\'s docstring)')
data = json.load(open(DATA, encoding='utf-8'))

sets = []
if (W / 'fl4sh_patches.json').exists():
    d = json.load(open(W / 'fl4sh_patches.json', encoding='utf-8'))
    for f in d['files']:
        n = f['dir']
        feats = ['resolution']
        if 'FPS' in n: feats = ['fps', 'timestep', 'physics']
        if 'Ultrawide' in n: feats = ['resolution', 'aspect', 'hud', 'fov']
        sets.append({'id': 'fl4sh/' + n, 'title': f['title_line'], 'author': 'Fl4sh_ (Fl4sh9174)', 'url': f['url'], 'repo': d['source'], 'commit': d['commit'],
                     'features': feats, 'header_comments': f['header_comments'], 'entries': f['entries']})
sets += data.get('sets', [])
out = {'meta': data.get('meta', {}), 'sets': sets, 'facts': data.get('facts', [])}

json.dump(out, open(ROOT / 'docs/mpr/patches.json', 'w', encoding='utf-8'), indent=1)

# snapshot text + symbol fragment ------------------------------------------------
snap, sym = [], []
def addsym(name, off, kind, role, tags, line, ev, mangled=None, contains=None):
    it = {'type': 'symbol', 'name': name, 'mangled': mangled, 'kind': kind, 'address': off, 'addr_form': 'main_offset' if off else 'unknown', 'claimed_version': '1.0.0',
          'signature': None, 'role': role, 'tags': tags, 'used_by': 'offset patch in community mod', 'src': {'path': 'work/kb/web/patches_snapshot.txt', 'line': line, 'evidence': ev}}
    if contains: it['contains'] = contains
    sym.append(it)
infn = collections.defaultdict(list)
seen = set()
for st in sets:
    snap.append(f'# {st["id"]} {st["url"]}')
    for e in st['entries']:
        snap.append(f'{e["offset"]} {e.get("bytes") or ""} // {e["comment"]}')
        ln = len(snap)
        c = e['comment']
        key = (e['offset'])
        m = re.search(r'(GetScreenWidth|GetScreenHeight)\s+(\w+::\w+)\s*$', c)
        if m: infn[m.group(2)].append((e['offset'], m.group(1), ln, f'{e["offset"]} {e.get("bytes") or ""} // {c}'))
        for nm, off, role in [('GetScreenWidth', '0x00001248', 'getter returning docked width constant (orig 1600, mov w0,#0x640; ret)'), ('GetScreenHeight', '0x00001250', 'getter returning docked height constant (orig 900)'),
                              ('HandheldFullWidth', '0x00001258', 'handheld width getter (orig 1024)'), ('HandheldFullHeight', '0x00001260', 'handheld height getter (orig 576)'),
                              ('HandheldContentWidth', '0x00001268', 'handheld content width getter (orig 1024)'), ('HandheldContentHeight', '0x00001270', 'handheld content height getter (orig 576)'),
                              ('DefaultThirdPersonVerticalFOV', '0x00c8c8ac', 'patched to 80.0 deg; function returns default 3rd-person vertical FOV'),
                              ('DefaultFirstPersonVerticalFOV', '0x00c8c8b8', 'patched to 74.0 deg (first-person vertical FOV default)'),
                              ('ResolutionScaleOrAspect hook (aspect constant load)', '0x00afe3d8', 'patch replaces aspect-ratio float load (1.75 or 2.3889 cave)'),
                              ('Dynamic resolution update (patched to ret)', '0x00aff5d4', 'disabling this function disables dynamic resolution')]:
            if e['offset'] == off and (nm, off) not in seen:
                seen.add((nm, off))
                addsym(nm, off, 'function' if off not in ('0x00afe3d8',) else 'patch_site', role, ['resolution' if 'FOV' not in nm else 'fov'], ln, e['offset'], None)
for fn, sites in infn.items():
    first = sites[0]
    addsym(fn, None, 'function', 'contains call sites of ' + ', '.join(sorted({s[1] for s in sites})) + ' patched in the ultrawide/res mods', ['resolution', 'hud'], first[2], first[0],
           contains=sorted({s[0].lower() for s in sites}))
(W).mkdir(parents=True, exist_ok=True)
(W / 'patches_snapshot.txt').write_text('\n'.join(snap) + '\n', encoding='utf-8')
json.dump({'source_id': 'web_patches', 'items': sym}, open(ROOT / 'docs/mpr/frag/web_patches.json', 'w', encoding='utf-8'), indent=1)
print('sets', len(sets), 'entries', sum(len(s['entries']) for s in sets), 'symbols', len(sym))
