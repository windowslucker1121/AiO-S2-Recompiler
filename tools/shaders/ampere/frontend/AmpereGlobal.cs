// SPDX-License-Identifier: MIT
// Global memory accesses whose address the translator cannot trace to one storage descriptor (A17, AGENTS.md). Ryujinx's GlobalToStorage
// dropped them (loads read 0, stores and atomics vanished); 3,485 of OoT's blocks had such accesses, among them the counters of its GPU
// culling, so every GPU-culled draw got zero work. Like the Lichii emulator (zortos293/Lichii, global_memory_to_storage_buffer_pass.cpp
// and DefineGlobalMemoryFunctions), such an access now checks at run time which bound storage buffer holds its address: the candidates are
// every storage descriptor slot of the stage's c[0] ({address, size} words), the access goes to the first range that contains it, and
// nothing matching still reads 0 / writes nothing. The harness sets the candidates per block (StaticGpuAccessor.GlobalFallbackWords) and
// adds the resource token "gmem" when a block uses them: the runtime then fills that stage's c[0] descriptors per draw.
// Hook: translator/spirv-g2s-c0-base.patch (GlobalToStorage.TryGenerateMultiTargetStorageOp).
using System;

namespace Ryujinx.Graphics.Shader.Translation
{
    public static class AmpereGlobal
    {
        [ThreadStatic] static int[] _candidates;
        [ThreadStatic] static bool _used;

        /// c[0] word offsets of the storage descriptors to check (null or empty: the access is dropped as before). Resets Used.
        public static void Begin(int[] candidateWords)
        {
            _candidates = candidateWords;
            _used = false;
        }

        public static int[] Candidates => _candidates;

        /// Whether the block being translated on this thread used the fallback.
        public static bool Used { get => _used; internal set => _used = value; }
    }
}
