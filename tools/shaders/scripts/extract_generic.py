#!/usr/bin/env python3
"""Game-independent NVN2 (platform 2) shader extractor: finds every NVN2 shader block in a romfs and the ExeFS modules by the
control section's signature, recovers its code area from the program record that points at it, and writes the Ampere
corpus layout the translator reads (the layout of the old KTGL extractor): code/<h>.bin, ctrl/<h>.bin,
glslc/<h>.bin (whole GLSLC output, when the block sits in one), manifest.tsv (one row per occurrence), unique.tsv (one row
per distinct code block), summary.txt. <h> = first 16 hex digits of SHA-1 of the bytes (code: the +0x44 bytes of the code
area, the key the runtime computes).

    python -I extract_generic.py --romfs <romfs> [--romfs <more>] [--main <exefs/main>] [--exefs <dir>] --out <dir>
                                 [--jobs 4] [--progress]

The library's setup runs it with --romfs --main --out --jobs --progress. --main also scans the other NSO modules next to it (sdk, subsdk*); --exefs scans
every NSO in a folder. Give each romfs once: a dumped update romfs is already the whole patched romfs.

Signature (docs/AMPERE.md section 3): u32 0x98761234 at +0, version u32 0xc0..0xff at +4 (0xc9 in every game seen),
control length u32 at +0x30 (0xd8 + optional tail) repeated at +0x98, program length +0x38, constant length/offset
+0x3c/+0x40, code area length +0x44, stage u16 +0x56.

Code area: every NVN2 producer seen keeps a program record {control offset, code offset, code length, control length} with
both offsets from one base: the GLSLC output's GPU code section header (u32 fields, base = section data; KTGL/title A) and
BNSH program records (u64 offsets, base = the 'BNSH' magic; the platform EPD/title B). The (code length, control length) pair
is searched next to each control section's own values (within 16 MiB). Each record votes for every base it could have
(control sections with its lengths, minus its control offset): the true base of a container gets a vote from each of its
records. The best-voted, then nearest, record whose code area passes the content check wins, with
every other record that names the same control section from the same base (BNSH shares one control section between
programs that differ only in code).
Content check: area inside the buffer and apart from the control section, program length <= code length, the bytes from
the program end to the constant data offset +0x40 (or the area end) are zero, first word non-zero, graphics stages start with an SPH
v4 (type 1 VTG / 2 PS). Without a record, code directly before/after the control section (0, 4 or 8 bytes apart: Unreal
writes {u32 code length, code}{u32 control length, control}), then 0x10-aligned within 0x1000, is tried with the same check.

Containers opened on the way (recursively, depth 6): zstd (frames anywhere; dictionaries from the romfs's *.zsdic and
ZsDic.pack), zlib (streams anywhere; streams that follow each other within 64 bytes are one buffer, so chunked formats
such as KTGL IDRK come out whole), gzip, LZ4 frame, Yaz0, NSO (SARC and similar archives need no parsing: their stored
members are scanned as bytes of the archive, compressed members are found as embedded frames) (LZ4 segments), Unreal IoStore (.utoc/.ucas: blocks,
shader library groups) and .pak (v10+ index) with Oodle (--oodle: an oozx DLL, a C ABI over the open oozextract crate;
else the `kraken_decompressor` package), zlib, LZ4
or no compression; length-prefixed Oodle blobs anywhere (Unreal shader map entries {TArray<uint8> Code, int32
UncompressedSize, uint8 Frequency}). Encrypted Unreal containers are reported and skipped.
Needs: lz4 (NSO, LZ4); zstandard or Python 3.14 (zstd); oozx or kraken_decompressor (Oodle). A missing module is reported once
per container type it is needed for, the rest of the scan goes on.
"""
import argparse, bisect, collections, hashlib, mmap, os, re, struct, sys, zlib
from concurrent.futures import ProcessPoolExecutor

CTRL_MAGIC = b'\x34\x12\x76\x98'
GLSLC_MAGIC = b'\x91\x68\x86\x19'
STAGES = {0: 'vs', 1: 'ps', 2: 'gs', 3: 'tcs', 4: 'tes', 5: 'cs', 6: 'stage6', 7: 'stage7'}
MAX_DEPTH = 6
ZLIB_GAP = 64           # streams closer than this are one buffer (KTGL: u16 length + 8 bytes between chunks)
MIN_BUF = 0x100         # decompressed buffers smaller than this cannot hold a control section and its code
ADJ_WINDOW = 0x1000
REC_WINDOW = 1 << 24    # a program record lies within 16 MiB of its control section

OUT = None
NOTES = collections.Counter()   # per worker: container types skipped and why
DICTS = {}                      # zstd dictionary id -> bytes


def low_priority():
    if os.name == 'nt':
        import ctypes
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)  # BELOW_NORMAL


# ---------------------------------------------------------------------------------------------------------------- codecs
def _zstd_module():
    try:
        from compression import zstd       # Python 3.14
        return 'std', zstd
    except ImportError:
        pass
    try:
        import zstandard
        return 'zstandard', zstandard
    except ImportError:
        return None, None


ZSTD_KIND, ZSTD = _zstd_module()


def zstd_frame(mv, p):
    """Decompress the zstd frame(s) starting at p. -> (data, end) or None."""
    if ZSTD is None:
        NOTES['zstd: no module (install zstandard or use Python 3.14)'] += 1
        return None
    head = bytes(mv[p:p + 18])
    try:
        if ZSTD_KIND == 'zstandard':
            did = ZSTD.get_frame_parameters(head).dict_id
        else:
            did = ZSTD.get_frame_info(head).dictionary_id
    except Exception:
        return None
    if did and did not in DICTS:
        NOTES[f'zstd: dictionary {did:#x} not found in the romfs'] += 1
        return None
    try:
        if ZSTD_KIND == 'zstandard':
            dd = ZSTD.ZstdDecompressor(dict_data=ZSTD.ZstdCompressionDict(DICTS[did])) if did else ZSTD.ZstdDecompressor()
            o = dd.decompressobj()
            out, pos, n = [], p, len(mv)
            while not o.eof and pos < n:
                piece = bytes(mv[pos:pos + (1 << 22)])
                out.append(o.decompress(piece))
                pos += len(piece)
            if not o.eof:
                return None
            return b''.join(out), pos - len(o.unused_data)
        dec = ZSTD.ZstdDecompressor(ZSTD.ZstdDict(DICTS[did])) if did else ZSTD.ZstdDecompressor()
        out, pos, n = [], p, len(mv)
        while not dec.eof and pos < n:
            piece = bytes(mv[pos:pos + (1 << 22)])
            out.append(dec.decompress(piece))
            pos += len(piece)
        if not dec.eof:
            return None
        return b''.join(out), pos - len(dec.unused_data)
    except Exception:
        return None


def zlib_stream(mv, p, wbits=15):
    """Inflate the zlib (or gzip with wbits 31) stream at p. -> (data, end) or None."""
    z = zlib.decompressobj(wbits)
    try:
        first = z.decompress(bytes(mv[p:p + 64]))
    except zlib.error:
        return None
    out, pos, n = [first], p + 64, len(mv)
    try:
        while not z.eof and pos < n:
            piece = bytes(mv[pos:pos + (1 << 20)])
            out.append(z.decompress(piece))
            pos += len(piece)
    except zlib.error:
        return None
    if not z.eof:
        return None
    return b''.join(out), min(pos, n) - len(z.unused_data)


def lz4_block(b, size):
    import lz4.block
    return lz4.block.decompress(b, uncompressed_size=size)


OODLE_DLL = None      # oozx build (C ABI over the open oozextract crate): --oodle, env NVN2_OODLE_DLL, or oozx.dll next to this script
_oodle_fn = None


def oodle(b, size):
    """Oodle (Kraken/Mermaid/Selkie/Leviathan) block -> bytes. oozx_decompress(src, n, dst, size) -> size or -1 from the
    oozx DLL when one is given; else the kraken_decompressor package (an older ooz: it rejects some Oodle 2.9 streams)."""
    global _oodle_fn
    if _oodle_fn is None:
        here = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'oozx.dll')
        path = OODLE_DLL or os.environ.get('NVN2_OODLE_DLL') or (here if os.path.exists(here) else None)
        if path:
            import ctypes
            lib = ctypes.CDLL(path)
            f = lib.oozx_decompress
            f.restype = ctypes.c_int64
            f.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
            def dec(src, n, f=f, ctypes=ctypes):
                out = ctypes.create_string_buffer(n)
                if f(src, len(src), out, n) != n:
                    raise ValueError('oodle: undecodable block')
                return out.raw
            _oodle_fn = dec
        else:
            import kraken_decompressor
            _oodle_fn = kraken_decompressor.decompress
    return _oodle_fn(bytes(b), size)


def yaz0(d):
    size = struct.unpack_from('>I', d, 4)[0]
    out = bytearray(size)
    s, o, n = 16, 0, len(d)
    while o < size and s < n:
        code = d[s]; s += 1
        for bit in range(8):
            if o >= size or s >= n:
                break
            if code & (0x80 >> bit):
                out[o] = d[s]; o += 1; s += 1
            else:
                b1, b2 = d[s], d[s + 1]; s += 2
                dist = ((b1 & 0xf) << 8 | b2) + 1
                cnt = b1 >> 4
                if cnt == 0:
                    cnt = d[s] + 0x12; s += 1
                else:
                    cnt += 2
                for _ in range(cnt):
                    out[o] = out[o - dist]; o += 1
    return bytes(out)


def nso_image(d):
    """NSO -> memory image of .text/.rodata/.data (header +0x10 segments {file off, mem off, size, x}, +0x60 compressed
    sizes, flag bits 0..2 = segment LZ4-compressed)."""
    flags, = struct.unpack_from('<I', d, 0xc)
    segs = [struct.unpack_from('<4I', d, 0x10 + 0x10 * i) for i in range(3)]
    csz = struct.unpack_from('<3I', d, 0x60)
    img = bytearray(max(m + s for _, m, s, _ in segs))
    for i, (fo, mo, sz, _) in enumerate(segs):
        seg = lz4_block(bytes(d[fo:fo + csz[i]]), sz) if flags & (1 << i) else bytes(d[fo:fo + sz])
        img[mo:mo + len(seg)] = seg
    return bytes(img)


def sarc_members(d):
    """SARC -> [(name, offset, bytes)]."""
    bom = d[6:8]
    e = '<' if bom == b'\xff\xfe' else '>'
    hl, = struct.unpack_from(e + 'H', d, 4)
    data_start, = struct.unpack_from(e + 'I', d, 0xc)
    if d[hl:hl + 4] != b'SFAT':
        return []
    n, = struct.unpack_from(e + 'H', d, hl + 6)
    nodes = hl + 0xc
    sfnt = nodes + 0x10 * n
    names = sfnt + 8
    res = []
    for i in range(n):
        h, attr, a, b = struct.unpack_from(e + 'IIII', d, nodes + 0x10 * i)
        name = ''
        if attr & 0x01000000:
            no = names + (attr & 0xffff) * 4
            name = bytes(d[no:d.index(b'\0', no)]).decode('utf-8', 'replace')
        name = name or f'{h:08x}'
        res.append((name, data_start + a, bytes(d[data_start + a:data_start + b])))
    return res


def load_dicts(romfs_list):
    """zstd dictionaries the romfs ships (*.zsdic files, ZsDic.pack[.zs] SARC of them): {dict id: bytes}."""
    found = {}
    def add(b):
        if len(b) >= 8 and b[:4] == b'\x37\xa4\x30\xec':
            found[struct.unpack_from('<I', b, 4)[0]] = b
    for root in romfs_list:
        for dp, _, fs in os.walk(root):
            for f in fs:
                low = f.lower()
                if not (low.endswith('.zsdic') or low.startswith('zsdic.pack')):
                    continue
                b = open(os.path.join(dp, f), 'rb').read()
                if b[:4] == b'\x28\xb5\x2f\xfd':
                    r = zstd_frame(memoryview(b), 0)
                    b = r[0] if r else b''
                if b[:4] == b'SARC':
                    for _, _, m in sarc_members(b):
                        add(m)
                else:
                    add(b)
    return found


# ------------------------------------------------------------------------------------------------------- NVN2 blocks
def ctrl_ok(d, h, n):
    if h + 0x100 > n and h + 0xd8 > n:
        return None
    ver, = struct.unpack_from('<I', d, h + 4)
    if not 0xc0 <= ver <= 0xff:
        return None
    C, = struct.unpack_from('<I', d, h + 0x30)
    if not 0xd8 <= C <= 0x4000 or h + C > n or struct.unpack_from('<I', d, h + 0x98)[0] != C:
        return None
    P, clen, coff, L = struct.unpack_from('<IIII', d, h + 0x38)
    st, = struct.unpack_from('<H', d, h + 0x56)
    if not 0 < L <= 0x4000000 or P > L or st > 15:
        return None
    return C, P, clen, coff, L, st


def zero_run(d, a, b):
    return b <= a or not any(memoryview(d)[a:b])


def find_blocks(d):
    """-> [(ctrl offset, ctrl bytes, code offset, code bytes, method, glslc offset, glslc bytes)] for one buffer."""
    n = len(d)
    ctrls = {}
    p = d.find(CTRL_MAGIC)
    while p >= 0:
        c = ctrl_ok(d, p, n)
        if c:
            ctrls[p] = c
        p = d.find(CTRL_MAGIC, p + 4)
    if not ctrls:
        return []
    # 1. GLSLC outputs: the GPU code section header names the control and code offsets exactly
    gl = glslc_outputs(d)
    gl_starts = [g[0] for g in gl]
    exact = {}
    for o, size, secs in gl:
        for base, co, ko, L, C in secs:
            if base + co in ctrls and ctrls[base + co][4] == L and ctrls[base + co][0] == C:
                exact[base + co] = base + ko
    # 2. program records elsewhere: {ctrl off, code off, code length, control length} next to each control section's own
    # (code length, control length), u32 or u64 offsets from a base the container's records share. Each record votes for
    # every base it could have (control sections with its lengths, minus its control offset); a true base gets one vote
    # from every record of its container, so the best-voted base picks the record of each control section.
    want = collections.defaultdict(list)
    for h, (C, P, clen, coff, L, st) in ctrls.items():
        if h not in exact:
            want[(L, C)].append(h)
    recs = collections.defaultdict(list)       # (L, C) -> [(record pos, form, ctrl off, code off)]
    for C in {k[1] for k in want}:
        pat = re.compile(re.escape(struct.pack('<I', C)))
        for m in pat.finditer(d):
            q = m.start()
            if q & 3 or q < 12:
                continue
            L, = struct.unpack_from('<I', d, q - 4)
            if (L, C) not in want:
                continue
            co, ko = struct.unpack_from('<II', d, q - 12)
            if co < n and ko < n:
                recs[(L, C)].append((q - 4, 32, co, ko))
            if q >= 20:
                co8, ko8 = struct.unpack_from('<QQ', d, q - 20)
                if co8 < n and ko8 < n:
                    recs[(L, C)].append((q - 4, 64, co8, ko8))
    votes = collections.Counter()
    for key, rl in recs.items():
        hs = sorted(want[key])
        for rp, form, co, ko in rl:
            for j in range(bisect.bisect_left(hs, rp - REC_WINDOW), bisect.bisect_right(hs, rp + REC_WINDOW)):
                if hs[j] >= co:
                    votes[(form, hs[j] - co)] += 1
    res = []
    for h in sorted(ctrls):
        C, P, clen, coff, L, st = ctrls[h]
        found = []
        if h in exact:
            found = [(exact[h], 'glslc')]
        else:
            cands = []
            near = []
            for rp, form, co, ko in recs.get((L, C), ()):
                if co <= h and abs(rp - h) <= REC_WINDOW:
                    near.append((form, co, ko))
                    cands.append((-votes[(form, h - co)], abs(rp - h), form, co, h - co + ko))
            cands.sort()
            for _, _, form, co, x in cands[:4096]:
                if code_ok(d, x, L, P, clen, coff, st, h, C, n):
                    # every record that names this control section from the same base is a program of its own
                    # (BNSH stores a control section once for programs that differ only in code)
                    xs = sorted({h - co + k for f, c, k in near if f == form and c == co})
                    found = [(y, f'rec{form}') for y in xs if code_ok(d, y, L, P, clen, coff, st, h, C, n)]
                    break
        if not found:
            for x in adjacent(d, h, C, L):
                if code_ok(d, x, L, P, clen, coff, st, h, C, n):
                    found = [(x, 'adjacent')]
                    break
        go, gb = 0, b''
        k = bisect.bisect_right(gl_starts, h) - 1
        if k >= 0 and gl[k][0] <= h < gl[k][0] + gl[k][1]:
            go = gl[k][0]
            gb = bytes(d[go:go + gl[k][1]])
        for x, how in found:
            res.append((h, bytes(d[h:h + C]), x, bytes(d[x:x + L]), how, go, gb))
        if not found:
            res.append((h, bytes(d[h:h + C]), -1, b'', 'no-code', go, gb))
    return res


def adjacent(d, h, C, L):
    """Code next to the control section: directly before or after it, or one/two u32 apart (Unreal's Sage shader code is
    two length-prefixed arrays, {u32 code length, code area}{u32 control length, control section}: code at h - 4 - L,
    not aligned), then 0x10-aligned positions within ADJ_WINDOW."""
    e = h + C
    x = h - 4 - L                  # both lengths confirmed: {u32 L, code}{u32 C, ctrl}
    if x >= 4 and struct.unpack_from('<I', d, h - 4)[0] == C and struct.unpack_from('<I', d, x - 4)[0] == L:
        yield x
    for k in (0, 4, 8):
        yield h - L - k
        yield e + k
    for k in range(0, ADJ_WINDOW, 0x10):
        yield ((e + 0xf) & ~0xf) + k
        yield ((h - L) & ~0xf) - k


def code_ok(d, x, L, P, clen, coff, st, h, C, n):
    if x < 0 or x + L > n or (x < h + C and h < x + L):
        return False
    if not any(d[x:x + 16]):
        return False
    end = coff if P <= coff <= L else L      # +0x40 can be below the area end with no constants (mesh-stage blocks)
    if not zero_run(d, x + P, x + end):
        return False
    if st <= 4:
        w0, = struct.unpack_from('<I', d, x)
        if (w0 & 0x1f) not in (1, 2) or (w0 >> 5) & 0x1f != 4:
            return False
    return True


def glslc_outputs(d):
    """NVN2 GLSLC outputs (docs/AMPERE.md section 2: u32 magic 0x19866891, +0x5c length, +0x64 section count, section
    header i at +0xa8 + 0x98*i = {u32 size, u32 offset from output, u32 type}; type 0 adds +0x30 control offset, +0x34 code
    offset (from the section data), +0x38 code length, +0x3c control length).
    -> [(offset, length, [(section data offset, ctrl off, code off, code length, ctrl length)])]"""
    res = []
    n = len(d)
    o = d.find(GLSLC_MAGIC)
    while o >= 0:
        if o + 0xa8 <= n:
            size, = struct.unpack_from('<I', d, o + 0x5c)
            nsec, = struct.unpack_from('<I', d, o + 0x64)
            if 0xa8 < size <= n - o and 0 < nsec <= 16 and 0xa8 + 0x98 * nsec <= size:
                secs = []
                for i in range(nsec):
                    sh = o + 0xa8 + 0x98 * i
                    ssz, soff, typ = struct.unpack_from('<3I', d, sh)
                    if typ != 0 or soff + ssz > size:
                        continue
                    co, ko, kn, cn = struct.unpack_from('<4I', d, sh + 0x30)
                    if co + cn <= ssz and ko + kn <= ssz and d[o + soff + co:o + soff + co + 4] == CTRL_MAGIC:
                        secs.append((o + soff, co, ko, kn, cn))
                if secs:          # a magic without a control section is no output (do not skip what it seems to cover)
                    res.append((o, size, secs))
                    o = d.find(GLSLC_MAGIC, o + size)
                    continue
        o = d.find(GLSLC_MAGIC, o + 4)
    return res


# ------------------------------------------------------------------------------------------------------------ scanning
ZLIB_RE = re.compile(rb'\x78[\x01\x5e\x9c\xda]|\x1f\x8b\x08|\x28\xb5\x2f\xfd|\x04\x22\x4d\x18|Yaz0')
ZHDR_RE = re.compile(rb'\x78[\x01\x5e\x9c\xda]')


def scan(d, src, base, depth, sink):
    """Scan one buffer: blocks in it, then every container inside it (decompressed and scanned in turn).
    src: source label; base: offset of this buffer in the file (for the manifest)."""
    for blk in find_blocks(d):
        sink(src, base, blk)
    if depth >= MAX_DEPTH:
        return
    n = len(d)
    mv = memoryview(d)
    if d[:4] == b'NSO0' and n >= 0x100:
        try:
            scan(nso_image(d), src + '!nso', base, depth + 1, sink)
        except ImportError:
            NOTES['nso: no lz4 module'] += 1
        except Exception:
            NOTES['nso: bad image'] += 1
        return
    if oodle_available() and d.find(CTRL_MAGIC) >= 0:
        scan_oodle_blobs(d, mv, src, base, depth, sink)
    p = 0
    while True:
        m = ZLIB_RE.search(d, p)
        if not m:
            break
        q = m.start()
        tag = d[q:q + 2]
        p = q + 1
        if tag[:1] == b'\x78':
            if q + 2 > n:
                continue
            r = zlib_stream(mv, q)
            if not r:
                continue
            parts, start, end = [r[0]], q, r[1]
            while True:                    # chunked zlib: the next stream begins within ZLIB_GAP bytes (chunk
                r2 = None                  # headers can hold a false 78 9c, so every candidate in the gap is tried)
                for nm in ZHDR_RE.finditer(d, end, min(n, end + ZLIB_GAP)):
                    r2 = zlib_stream(mv, nm.start())
                    if r2:
                        break
                if not r2:
                    break
                parts.append(r2[0]); end = r2[1]
            buf = b''.join(parts)
            if len(buf) >= MIN_BUF:
                scan(buf, f'{src}!zlib@{start:#x}', base if base else start, depth + 1, sink)
            p = end
        elif tag == b'\x1f\x8b':
            r = zlib_stream(mv, q, 31)
            if r:
                if len(r[0]) >= MIN_BUF:
                    scan(r[0], f'{src}!gz@{q:#x}', base if base else q, depth + 1, sink)
                p = r[1]
        elif tag == b'\x28\xb5':
            r = zstd_frame(mv, q)
            if r:
                if len(r[0]) >= MIN_BUF:
                    scan(r[0], f'{src}!zs@{q:#x}' if q else src + '!zs', base if base else q, depth + 1, sink)
                p = r[1]
        elif tag == b'\x04\x22':
            try:
                import lz4.frame
                o = lz4.frame.LZ4FrameDecompressor()
                out = o.decompress(bytes(mv[q:]))
                if o.eof and len(out) >= MIN_BUF:
                    scan(out, f'{src}!lz4@{q:#x}', base if base else q, depth + 1, sink)
                    p = n - len(o.unused_data)
            except ImportError:
                NOTES['lz4 frame: no lz4 module'] += 1
            except Exception:
                pass
        elif tag == b'Ya' and q + 16 <= n:
            size, = struct.unpack_from('>I', d, q + 4)
            if MIN_BUF <= size <= 1 << 31 and q % 4 == 0:
                try:
                    out = yaz0(mv[q:])
                    scan(out, f'{src}!yaz0@{q:#x}', base if base else q, depth + 1, sink)
                except Exception:
                    pass


OODLE_RE = re.compile(rb'[\x8c\xcc][\x00-\x0f]')


def oodle_available():
    try:
        oodle(b'', 0)
    except ImportError:
        NOTES['oodle: no oozx DLL or kraken_decompressor module'] += 1
        return False
    except Exception:
        pass
    return True


def scan_oodle_blobs(d, mv, src, base, depth, sink):
    """Length-prefixed Oodle blobs: {i32 compressed length n, n bytes of Oodle (first byte 0x8c/0xcc), i32 uncompressed
    length}: Unreal's FShaderMapResourceCode shader entries {TArray<uint8> Code, int32 UncompressedSize, uint8 Frequency}
    (shader maps inline in packages and in GlobalShaderCache-*.bin). Only buffers that show a control magic are searched:
    Oodle keeps literals, so a compressed control section still shows its first bytes."""
    n = len(d)
    p = 4
    while True:
        m = OODLE_RE.search(d, p)
        if not m:
            return
        q = m.start()
        p = q + 1
        if q < 4:
            continue
        k, = struct.unpack_from('<i', d, q - 4)
        if not 2 < k <= n - q - 4:
            continue
        u, = struct.unpack_from('<i', d, q + k)
        if not k < u <= min(1 << 26, 64 * k + 0x1000):
            continue
        try:
            out = oodle(mv[q:q + k], u)
        except Exception:
            continue
        if len(out) >= MIN_BUF:
            scan(out, f'{src}!oodle@{q:#x}', base if base else q, depth + 1, sink)
        p = q + k


# ------------------------------------------------------------------------------------------------------------ Unreal
def utoc_read(path):
    d = open(path, 'rb').read()
    if d[:16] != b'-==--==--==--==-':
        return None
    ver = d[16]
    hsz, nent, nblk, bsz, ncm, cml, blksz, disz, nparts = struct.unpack_from('<9I', d, 20)
    flags = d[80]
    nseeds, = struct.unpack_from('<I', d, 84)
    psize, = struct.unpack_from('<Q', d, 88)
    nnoph, = struct.unpack_from('<I', d, 96)
    o = hsz
    ids = [d[o + 12 * i:o + 12 * i + 12] for i in range(nent)]; o += 12 * nent
    ol = []
    for i in range(nent):
        b = d[o + 10 * i:o + 10 * i + 10]
        ol.append((int.from_bytes(b[:5], 'big'), int.from_bytes(b[5:], 'big')))
    o += 10 * nent
    if ver >= 4:
        o += 4 * nseeds
    if ver >= 5:
        o += 4 * nnoph
    blocks = []
    for i in range(nblk):
        b = d[o + bsz * i:o + bsz * i + 12]
        blocks.append((int.from_bytes(b[:5], 'little'), int.from_bytes(b[5:8], 'little'), int.from_bytes(b[8:11], 'little'), b[11]))
    o += bsz * nblk
    methods = ['None'] + [d[o + cml * i:o + cml * (i + 1)].split(b'\0')[0].decode() for i in range(ncm)]
    return dict(version=ver, flags=flags, ids=ids, ol=ol, blocks=blocks, methods=methods, block_size=blksz,
                partition_size=psize or (1 << 62))


def decode_block(raw, usize, method):
    m = method.lower()
    if m == 'none':
        return raw[:usize]
    if m == 'oodle':
        return oodle(raw, usize)
    if m == 'zlib':
        return zlib.decompress(raw)
    if m == 'lz4':
        return lz4_block(raw, usize)
    raise ValueError('compression method ' + method)


class Ucas:
    def __init__(self, utoc, toc):
        self.toc = toc
        stem = utoc[:-5]
        self.parts = []
        for i in range(64):
            p = stem + ('.ucas' if i == 0 else f'_s{i}.ucas')
            if not os.path.exists(p):
                break
            f = open(p, 'rb')
            self.parts.append(mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ))

    def chunk(self, idx):
        off, ln = self.toc['ol'][idx]
        bs = self.toc['block_size']
        out = []
        b0, b1 = off // bs, (off + ln + bs - 1) // bs
        for b in range(b0, b1):
            coff, csz, usz, mi = self.toc['blocks'][b]
            part = self.parts[coff // self.toc['partition_size']]
            po = coff % self.toc['partition_size']
            raw = bytes(part[po:po + ((csz + 15) & ~15 if self.toc['flags'] & 2 else csz)])[:csz]
            out.append(decode_block(raw, usz, self.toc['methods'][mi]))
        data = b''.join(out)
        s = off - b0 * bs
        return data[s:s + ln]


def shader_library(lib):
    """FIoStoreShaderCodeArchive (chunk type ShaderCodeLibrary): u32 version, then arrays (i32 count + items):
    shader map hashes (20 B), shader hashes (20 B), group chunk ids (12 B), map entries (8 B), shader entries (8 B),
    group entries {u32 indices offset, u32 shader count, u32 uncompressed size, u32 compressed size}, indices (4 B).
    -> [(group chunk id, uncompressed size, compressed size)]"""
    o = 4
    def arr(sz):
        nonlocal o
        n, = struct.unpack_from('<i', lib, o)
        a = o + 4
        o = a + n * sz
        return n, a
    arr(20); arr(20)
    ng, ga = arr(12)
    arr(8); arr(8)
    ne, ea = arr(16)
    if ng != ne:
        return None
    res = []
    for i in range(ng):
        _, _, us, cs = struct.unpack_from('<4I', lib, ea + 16 * i)
        res.append((lib[ga + 12 * i:ga + 12 * i + 12], us, cs))
    return res


def scan_iostore(path, label, sink):
    toc = utoc_read(path)
    if toc is None:
        return
    if toc['flags'] & 2:
        NOTES[f'iostore {os.path.basename(path)}: encrypted (AES key needed)'] += 1
        return
    uc = Ucas(path, toc)
    if not uc.parts:
        return
    index = {cid: i for i, cid in enumerate(toc['ids'])}
    types = collections.Counter(cid[11] for cid in toc['ids'])
    libs = [i for i, cid in enumerate(toc['ids']) if cid[11] == 8]
    group_ids = set()
    for li in libs:
        try:
            lib = uc.chunk(li)
            groups = shader_library(lib)
        except Exception as e:
            NOTES[f'iostore: shader library unreadable ({type(e).__name__}: {e})'] += 1
            continue
        if groups is None:
            NOTES['iostore: shader library layout not recognised'] += 1
            continue
        for gi, (cid, us, cs) in enumerate(groups):
            i = index.get(cid)
            if i is None:
                NOTES['iostore: shader group chunk missing'] += 1
                continue
            group_ids.add(i)
            try:
                g = uc.chunk(i)
                if cs < us:
                    g = oodle(g, us)
            except ImportError:
                NOTES['oodle: no kraken_decompressor module'] += 1
                return
            except Exception as e:
                NOTES[f'iostore: shader group undecodable ({type(e).__name__})'] += 1
                continue
            scan(g, f'{label}!lib{li}/group{gi}', 0, 1, sink)
    # every other chunk except the shader groups already done (shader code can also sit in package/bulk data)
    for i, cid in enumerate(toc['ids']):
        if i in group_ids or cid[11] == 8:
            continue
        try:
            c = uc.chunk(i)
        except ImportError:
            NOTES['oodle: no kraken_decompressor module'] += 1
            return
        except Exception as e:
            NOTES[f'iostore: chunk undecodable ({type(e).__name__})'] += 1
            continue
        if len(c) >= MIN_BUF:
            scan(c, f'{label}!chunk{i}.t{cid[11]}', 0, 1, sink)


def pak_names(fd):
    """Full directory index: i32 dirs, each {FString dir, i32 files, each {FString name, i32 encoded entry offset}}."""
    def fstr(o):
        l, = struct.unpack_from('<i', fd, o); o += 4
        if l < 0:
            return fd[o:o - 2 * l].decode('utf-16-le').rstrip('\0'), o - 2 * l
        return fd[o:o + l].rstrip(b'\0').decode('utf-8', 'replace'), o + l
    res, o = {}, 0
    nd, = struct.unpack_from('<i', fd, o); o += 4
    for _ in range(nd):
        dn, o = fstr(o)
        nf, = struct.unpack_from('<i', fd, o); o += 4
        for _ in range(nf):
            fn, o = fstr(o)
            eo, = struct.unpack_from('<i', fd, o); o += 4
            res[eo] = dn + fn
    return res


def scan_pak(path, label, sink):
    """Unreal .pak v8..v11: footer {guid 16, u8 encrypted index, u32 magic 0x5a6f12e1, u32 version, u64 index offset,
    u64 index size, 20 B hash, compression method names 32 B each}. v10+: path-hash index + encoded entries (UE
    FPakFile::EncodePakEntry bit layout); an entry's data follows its serialized FPakEntry header."""
    with open(path, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as d:
        n = len(d)
        foot = None
        for k in range(5, -1, -1):           # number of compression names: 5 (v8b+) .. 0
            fo = n - (0x2c + 16 + 1 + 32 * k)
            if fo >= 0 and d[fo + 17:fo + 21] == b'\xe1\x12\x6f\x5a':
                foot = fo, k
                break
        if not foot:
            return
        fo, k = foot
        enc = d[fo + 16]
        ver, ioff, isz = struct.unpack_from('<IQQ', d, fo + 21)
        methods = ['None'] + [bytes(d[fo + 0x2c + 16 + 1 + 32 * i:fo + 0x2c + 17 + 32 * (i + 1)]).split(b'\0')[0].decode()
                              for i in range(k)]
        if enc:
            NOTES[f'pak {os.path.basename(path)}: encrypted index (AES key needed)'] += 1
            return
        if ver < 10:
            NOTES[f'pak {os.path.basename(path)}: version {ver} index not supported'] += 1
            return
        idx = bytes(d[ioff:ioff + isz])
        o = 0
        ml, = struct.unpack_from('<i', idx, o); o += 4 + ml          # mount point FString
        count, = struct.unpack_from('<i', idx, o); o += 4
        o += 8                                                       # path hash seed
        has_phi, = struct.unpack_from('<i', idx, o); o += 4
        if has_phi:
            o += 8 + 8 + 20
        has_fdi, = struct.unpack_from('<i', idx, o); o += 4
        names = {}
        if has_fdi:
            fdo, fds = struct.unpack_from('<QQ', idx, o)
            o += 8 + 8 + 20
            try:
                names = pak_names(bytes(d[fdo:fdo + fds]))
            except Exception:
                names = {}
        esz, = struct.unpack_from('<i', idx, o); o += 4
        enc_entries = idx[o:o + esz]; o += esz
        nfiles, = struct.unpack_from('<i', idx, o); o += 4
        # encoded entries: decode them all in order (each self-delimiting)
        e = 0
        while e < len(enc_entries):
            name = names.get(e)
            v, = struct.unpack_from('<I', enc_entries, e); e += 4
            ci = (v >> 23) & 0x3f
            bsize = (v & 0x3f) << 11
            if (v & 0x3f) == 0x3f:
                bsize, = struct.unpack_from('<I', enc_entries, e); e += 4
            if v & (1 << 31):
                off, = struct.unpack_from('<I', enc_entries, e); e += 4
            else:
                off, = struct.unpack_from('<Q', enc_entries, e); e += 8
            if v & (1 << 30):
                us, = struct.unpack_from('<I', enc_entries, e); e += 4
            else:
                us, = struct.unpack_from('<Q', enc_entries, e); e += 8
            cs = us
            if ci:
                if v & (1 << 29):
                    cs, = struct.unpack_from('<I', enc_entries, e); e += 4
                else:
                    cs, = struct.unpack_from('<Q', enc_entries, e); e += 8
            encrypted = bool(v & (1 << 22))
            nblocks = (v >> 6) & 0xffff
            blocks = []
            if ci and nblocks:
                # header = FPakEntry serialized size (v10+, no hash/timestamp in this form): computed per UE source
                hdr = 8 + 8 + 8 + 4 + 20 + 4 + 16 * nblocks + 1 + 4
                if nblocks == 1 and not encrypted:
                    blocks = [(hdr, hdr + cs)]
                else:
                    pos = hdr
                    for _ in range(nblocks):
                        bs_, = struct.unpack_from('<I', enc_entries, e); e += 4
                        blocks.append((pos, pos + bs_))
                        pos += (bs_ + 15) & ~15 if encrypted else bs_
            if encrypted:
                NOTES['pak: encrypted entries (AES key needed)'] += 1
                continue
            try:
                if not ci:
                    hdr = 8 + 8 + 8 + 4 + 20 + 1 + 4
                    data = bytes(d[off + hdr:off + hdr + us])
                else:
                    meth = methods[ci] if ci < len(methods) else '?'
                    parts, left = [], us
                    for a, b in blocks:
                        u = min(bsize or us, left)
                        parts.append(decode_block(bytes(d[off + a:off + b]), u, meth))
                        left -= u
                    data = b''.join(parts)
            except ImportError:
                NOTES['oodle: no kraken_decompressor module'] += 1
                return
            except Exception as ex:
                NOTES[f'pak: entry undecodable ({type(ex).__name__}: {str(ex)[:60]})'] += 1
                continue
            if len(data) >= MIN_BUF:
                scan(data, f'{label}!{name or hex(off)}', off, 1, sink)


# ------------------------------------------------------------------------------------------------------------- output
def put(sub, h, data):
    """Content-addressed write (same name = same bytes, so parallel workers cannot disagree)."""
    p = os.path.join(OUT, sub, h + '.bin')
    if not os.path.exists(p):
        t = f'{p}.{os.getpid()}.tmp'
        with open(t, 'wb') as f:
            f.write(data)
        try:
            os.replace(t, p)
        except PermissionError:
            if not os.path.exists(p):
                raise
            os.remove(t)


def h16(b):
    return hashlib.sha1(b).hexdigest()[:16]


def run_task(task, out, dicts, oodle_dll=None):
    """One work item -> (rows, notes). task = (kind, path, label)."""
    global OUT, DICTS, OODLE_DLL
    OUT, DICTS, OODLE_DLL = out, dicts, oodle_dll
    NOTES.clear()
    low_priority()
    rows = []

    def sink(src, base, blk):
        h, ctrl, x, code, how, go, gb = blk
        ch = h16(ctrl)
        put('ctrl', ch, ctrl)
        st = STAGES.get(struct.unpack_from('<H', ctrl, 0x56)[0], 'stage%d' % struct.unpack_from('<H', ctrl, 0x56)[0])
        gh = '-'
        if gb:
            gh = h16(gb); put('glslc', gh, gb)
        kh = '-'
        if code:
            kh = h16(code); put('code', kh, code)
        rows.append((src, base, h, how, go, st, len(ctrl), len(code), kh, ch, gh))

    kind, path, label = task
    try:
        if kind == 'iostore':
            scan_iostore(path, label, sink)
        elif kind == 'pak':
            scan_pak(path, label, sink)
        else:
            with open(path, 'rb') as f:
                if os.fstat(f.fileno()).st_size == 0:
                    return rows, dict(NOTES)
                with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as d:
                    scan(d, label, 0, 0, sink)
    except MemoryError:
        NOTES[f'{label}: out of memory'] += 1
    return rows, dict(NOTES)


def collect(romfs_list, mains, exefs_dirs):
    tasks = []
    seen_rel = set()
    for root in romfs_list:
        files = []
        for dp, _, fs in os.walk(root):
            for f in fs:
                files.append(os.path.join(dp, f))
        utocs = {p[:-5] for p in files if p.lower().endswith('.utoc')}
        for p in sorted(files, key=lambda x: os.path.relpath(x, root).replace(os.sep, '/')):
            rel = os.path.relpath(p, root).replace(os.sep, '/')
            if rel in seen_rel:
                continue
            seen_rel.add(rel)
            low = p.lower()
            if low.endswith('.utoc'):
                tasks.append(('iostore', p, rel))
            elif low.endswith('.ucas') and re.sub(r'(_s\d+)?\.ucas$', '', p, flags=re.I) in utocs:
                continue                       # read through its .utoc
            elif low.endswith('.pak'):
                tasks.append(('pak', p, rel))
            else:
                tasks.append(('file', p, rel))
    mods = []
    for m in mains:
        mods.append(m)
        dp = os.path.dirname(os.path.abspath(m))
        for f in sorted(os.listdir(dp)):
            q = os.path.join(dp, f)
            if os.path.isfile(q) and q != os.path.abspath(m) and open(q, 'rb').read(4) == b'NSO0':
                mods.append(q)
    for dp in exefs_dirs:
        for f in sorted(os.listdir(dp)):
            q = os.path.join(dp, f)
            if os.path.isfile(q) and open(q, 'rb').read(4) == b'NSO0':
                mods.append(q)
    seen_mod = set()
    for q in mods:
        k = os.path.normcase(os.path.abspath(q))
        if k not in seen_mod:
            seen_mod.add(k)
            tasks.append(('file', q, os.path.basename(q)))
    return tasks


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--romfs', action='append', default=[], help='romfs folder (repeatable; a path seen in an earlier one wins)')
    ap.add_argument('--main', action='append', default=[], help='ExeFS main (NSO or decompressed); its sibling NSOs are scanned too')
    ap.add_argument('--exefs', action='append', default=[], help='ExeFS folder: every NSO in it')
    ap.add_argument('--out', required=True)
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--progress', action='store_true', help='print "scan: <done> <total>" lines while scanning')
    ap.add_argument('--oodle', help='oozx DLL for Oodle (default: env NVN2_OODLE_DLL, then oozx.dll next to this script, else the kraken_decompressor package)')
    a = ap.parse_args()
    for sub in ('code', 'ctrl', 'glslc'):
        os.makedirs(os.path.join(a.out, sub), exist_ok=True)
    dicts = load_dicts(a.romfs)
    tasks = collect(a.romfs, a.main, a.exefs)
    sizes = []
    for k, p, _ in tasks:
        s = os.path.getsize(p)
        if k == 'iostore':
            s += sum(os.path.getsize(q) for q in [p[:-5] + '.ucas'] if os.path.exists(q))
        sizes.append(s)
    order = sorted(range(len(tasks)), key=lambda i: -sizes[i])     # biggest first, results kept in task order
    results = [None] * len(tasks)
    notes = collections.Counter()
    with ProcessPoolExecutor(max(1, a.jobs)) as ex:
        futs = {}
        for i in order:
            futs[ex.submit(run_task, tasks[i], a.out, dicts, a.oodle)] = i
        done = 0
        from concurrent.futures import as_completed
        for f in as_completed(futs):
            results[futs[f]] = f.result()
            done += 1
            if a.progress:
                print(f'scan: {done} {len(tasks)}', flush=True)
    for _, nt in results:
        notes.update(nt)
    seen, occ, variants = {}, collections.Counter(), collections.defaultdict(set)
    rows, nocode, methods = [], [], collections.Counter()
    for res, _ in results:
        for src, base, h, how, go, st, cn, kn, kh, ch, gh in res:
            methods[how] += 1
            rows.append(f'{src}\t{base:#x}\t{h:#x}\t{how}\t{go:#x}\t{st}\t{cn:#x}\t{kn:#x}\t{kh}\t{ch}\t{gh}')
            if kh == '-':
                nocode.append((src, h, st))
                continue
            occ[kh] += 1
            variants[kh].add(ch)
            if kh not in seen:
                seen[kh] = (st, kn, cn, src, base, go if gh != '-' else h, ch, gh)
    with open(os.path.join(a.out, 'manifest.tsv'), 'w', newline='\n', encoding='utf-8') as f:
        f.write('source\tbuffer_off\tctrl_off\tmethod\tglslc_off\tstage\tctrl_size\tcode_size\tcode_sha1_16\tctrl_sha1_16\tglslc_sha1_16\n')
        f.write(''.join(r + '\n' for r in rows))
    # unique.tsv: the corpus layout's columns. first_source = the source label (romfs path, with !<container> steps;
    # "main" for the ExeFS main), entry_off = offset of the decompressed buffer in the file, glslc_off = GLSLC output (or
    # control section) offset in that buffer, first_glslc '-' when the block was not inside a GLSLC output.
    with open(os.path.join(a.out, 'unique.tsv'), 'w', newline='\n', encoding='utf-8') as f:
        f.write('code_sha1_16\tstage\tcode_size\tctrl_size\toccurrences\tctrl_variants\tfirst_source\tentry_off\tglslc_off'
                '\tfirst_ctrl\tfirst_glslc\n')
        for h in sorted(seen):
            st, kn, cn, src, base, go, ch, gh = seen[h]
            f.write(f'{h}\t{st}\t{kn:#x}\t{cn:#x}\t{occ[h]}\t{len(variants[h])}\t{src}\t{base:#x}\t{go:#x}\t{ch}\t{gh}\n')
    by_all = collections.Counter(r.split('\t')[5] for r in rows if r.split('\t')[8] != '-')
    by_uni = collections.Counter(v[0] for v in seen.values())
    s = [f'occurrences {sum(occ.values())}  unique code blocks {len(seen)}  control sections without code {len(nocode)}']
    for st in sorted(set(by_all) | set(by_uni)):
        s.append(f'  {st}: {by_all[st]} occurrences, {by_uni[st]} unique')
    s.append('code found by: ' + ', '.join(f'{k} {v}' for k, v in sorted(methods.items())))
    top = collections.Counter(r.split('\t')[0].split('!')[0] for r in rows)
    s.append('top sources (occurrences): ' + ', '.join(f'{k} {v}' for k, v in top.most_common(12)))
    if nocode:
        s.append('control sections without code (first 10): ' + ', '.join(f'{a_}@{b_:#x}({c_})' for a_, b_, c_ in nocode[:10]))
    if notes:
        s.append('notes: ' + '; '.join(f'{k} x{v}' for k, v in sorted(notes.items())))
    open(os.path.join(a.out, 'summary.txt'), 'w', newline='\n', encoding='utf-8').write('\n'.join(s) + '\n')
    print('\n'.join(s))


if __name__ == '__main__':
    main()
