#!/usr/bin/env python3
"""Writes the entry-point coverage table of docs/S4d.md.

    set NXRT_NVN_DUMP_COVERAGE=work\\nvn\\coverage.tsv ; build\\release\\nxrt_gfx_tests.exe gfx_nvn_impl      (the test dumps name, ret, args, sources, implemented)
    python scripts\\gen_nvn_coverage_doc.py work\\nvn\\coverage.tsv [work\\nvn\\mpr_used_nvn.txt] > docs\\S4d_coverage.md

mpr_used_nvn.txt (optional) lists the NVN functions that title E calls (names with callers in main.elf outside
its function loader, from the IDA database); those rows get a 'game' mark.
"""
import csv, re, sys

# Implemented, but approximated or ignored: what the function really does.
PARTIAL = {
    'nvnCommandBufferReportCounter': 'timestamp counters are real; occlusion/statistics counters report a non-zero count (everything visible)',
    'nvnCommandBufferResetCounter': 'ignored',
    'nvnCommandBufferWaitEvent': 'not executed on the GPU (single in-order queue); counted as unsupported',
    'nvnCommandBufferSignalEvent': 'SET and ADD are written into event memory in GPU order; the location argument is ignored',
    'nvnCommandBufferBeginTransformFeedback': 'recorded, not executed (no transform feedback yet)',
    'nvnCommandBufferEndTransformFeedback': 'recorded, not executed (no transform feedback yet)',
    'nvnCommandBufferPauseTransformFeedback': 'no-op',
    'nvnCommandBufferResumeTransformFeedback': 'no-op',
    'nvnCommandBufferDrawTransformFeedback': 'no-op',
    'nvnCommandBufferBindTransformFeedbackBuffer': 'recorded, ignored',
    'nvnCommandBufferBindTransformFeedbackBuffers': 'recorded, ignored',
    'nvnCommandBufferDrawTexture': 'recorded, skipped at replay (counted as unsupported)',
    'nvnCommandBufferSetPatchSize': 'recorded, ignored (no tessellation)',
    'nvnCommandBufferSetInnerTessellationLevels': 'recorded, ignored (no tessellation)',
    'nvnCommandBufferSetOuterTessellationLevels': 'recorded, ignored (no tessellation)',
    'nvnCommandBufferSetShaderScratchMemory': 'ignored (the GPU manages scratch)',
    'nvnCommandBufferSaveZCullData': 'no-op', 'nvnCommandBufferRestoreZCullData': 'no-op',
    'nvnCommandBufferSetTiledCacheAction': 'no-op', 'nvnCommandBufferSetTiledCacheTileSize': 'no-op',
    'nvnCommandBufferSetConservativeRasterEnable': 'recorded, ignored', 'nvnCommandBufferSetConservativeRasterDilate': 'no-op',
    'nvnCommandBufferSetSubpixelPrecisionBias': 'no-op', 'nvnCommandBufferSetViewportSwizzles': 'no-op',
    'nvnCommandBufferSetAlphaRef': 'stored; the alpha test itself is not applied (the game does not enable it)',
    'nvnCommandBufferSetPointSize': 'stored; the vertex shader writes the size',
    'nvnCommandBufferResolveDepthBuffer': 'recorded, skipped',
    'nvnCommandBufferDownsample': 'colour only (vkCmdResolveImage); depth resolve is not implemented',
    'nvnCommandBufferTiledDownsample': 'colour only', 'nvnCommandBufferDownsampleTextureView': 'colour only', 'nvnCommandBufferTiledDownsampleTextureView': 'colour only',
    'nvnCommandBufferBindSeparateTexture': 'no-op', 'nvnCommandBufferBindSeparateSampler': 'no-op',
    'nvnCommandBufferBindSeparateTextures': 'no-op', 'nvnCommandBufferBindSeparateSamplers': 'no-op',
    'nvnCommandBufferPushDebugGroup': 'no-op', 'nvnCommandBufferPopDebugGroup': 'no-op', 'nvnCommandBufferInsertDebugMarker': 'no-op',
    'nvnMemoryPoolMapVirtual': 'returns 0: sparse virtual pools are not implemented (the game calls it for virtual textures)',
    'nvnQueueWaitSync': 'no-op: one in-order GPU queue',
    'nvnDeviceSetIntermediateShaderCache': 'ignored', 'nvnDeviceWalkDebugDatabase': 'no-op', 'nvnDeviceInstallDebugCallback': 'no-op',
    'nvnDeviceRegisterFastClearColor': 'accepted, clears are exact anyway', 'nvnDeviceRegisterFastClearDepth': 'accepted',
    'nvnProgramSetSubroutineLinkage': 'accepted', 'nvnProgramSetSampleShading': 'accepted (SampleId in a shader already forces sample shading)',
    'nvnTextureGetSparseTileLayout': 'zeros', 'nvnTextureBuilderGetSparseTileLayout': 'zeros',
}


def family(name):
    m = re.match(r'nvn([A-Z][a-z]+(?:[A-Z][a-z]+)*?)(?=Set|Get|Bind|Initialize|Finalize|Is|Update|Add|Draw|Clear|Copy|Resolve|Insert|Push|Pop|Submit|Present|Acquire|Flush|Finish|Wait|Fence|Signal|Report|Reset|Free|Map|Invalidate|Register|Dispatch|Call|End|Begin|Download|Write|Read|Default|Install|Walk|Apply|Generate|Discard|Compare|Save|Restore|Pause|Resume|Tiled|Barrier|Downsample|Bootstrap|Load|Create|$)', name)
    f = m.group(1) if m else name[3:]
    return f


def main():
    rows = list(csv.DictReader(open(sys.argv[1]), delimiter='\t'))
    used = set(open(sys.argv[2]).read().split()) if len(sys.argv) > 2 else set()
    fams = {}
    for r in rows:
        fams.setdefault(family(r['name']), []).append(r)
    impl = sum(1 for r in rows if r['implemented'] == '1')
    print('## Entry-point coverage')
    print()
    print(f'{impl} of {len(rows)} NVN entry points have an implementation; {len(rows) - impl} are still logging stubs. '
          f'{sum(1 for r in rows if r["name"] in used)} entry points are called by the game' + (' (all implemented)' if all(r['implemented'] == '1' for r in rows if r['name'] in used) else '') + '.')
    print()
    print('Status: `real` = implemented as specified; `partial` = implemented but approximated or ignored (note says how); `stub` = still the logging stub. '
          '`game` = the game binary calls it.')
    print()
    print('| Family | Functions | real | partial | stub | Not real or not implemented |')
    print('|---|---:|---:|---:|---:|---|')
    detail = []
    for f in sorted(fams):
        rs = fams[f]
        real = [r for r in rs if r['implemented'] == '1' and r['name'] not in PARTIAL]
        part = [r for r in rs if r['implemented'] == '1' and r['name'] in PARTIAL]
        stub = [r for r in rs if r['implemented'] != '1']
        note = ', '.join(sorted(r['name'][3:] for r in part + stub))
        print(f'| {f} | {len(rs)} | {len(real)} | {len(part)} | {len(stub)} | {note if len(note) < 300 else str(len(part) + len(stub)) + " (listed below)"} |')
        for r in part:
            detail.append((r['name'], 'partial', 'game' if r['name'] in used else '', PARTIAL[r['name']]))
        for r in stub:
            detail.append((r['name'], 'stub', 'game' if r['name'] in used else '', 'logging stub'))
    print()
    print('### Partial and stub functions')
    print()
    print('| Function | Status | Game | What it does |')
    print('|---|---|---|---|')
    for d in sorted(detail):
        print('| %s | %s | %s | %s |' % d)


if __name__ == '__main__':
    main()
