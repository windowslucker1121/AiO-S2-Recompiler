// SPDX-License-Identifier: MIT
// Translator side of the oracle: runs a spirv_harness --hwcheck module in Vulkan compute on the same GPU.
// Raw P/Invoke on vulkan-1.dll (no SDK or binding package needed). Descriptor layout as the translator declares it:
// set 0 binding 0 = support buffer (uniform, zero-filled), set 1 binding 0 = the register-file storage buffer.
// The device enables every feature the driver reports (Vulkan 1.0-1.3 feature structs) so any capability the
// module declares is available.
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Hwcheck
{
    sealed unsafe class VkRunner : IDisposable
    {
        const string Dll = "vulkan-1.dll";
        [DllImport(Dll)] static extern int vkCreateInstance(void* ci, void* alloc, out IntPtr inst);
        [DllImport(Dll)] static extern void vkDestroyInstance(IntPtr inst, void* alloc);
        [DllImport(Dll)] static extern int vkEnumeratePhysicalDevices(IntPtr inst, ref uint count, IntPtr* devs);
        [DllImport(Dll)] static extern void vkGetPhysicalDeviceProperties(IntPtr pd, byte* props);
        [DllImport(Dll)] static extern void vkGetPhysicalDeviceQueueFamilyProperties(IntPtr pd, ref uint count, byte* props);
        [DllImport(Dll)] static extern void vkGetPhysicalDeviceFeatures2(IntPtr pd, byte* f2);
        [DllImport(Dll)] static extern void vkGetPhysicalDeviceMemoryProperties(IntPtr pd, byte* mp);
        [DllImport(Dll)] static extern int vkCreateDevice(IntPtr pd, void* ci, void* alloc, out IntPtr dev);
        [DllImport(Dll)] static extern void vkDestroyDevice(IntPtr dev, void* alloc);
        [DllImport(Dll)] static extern void vkGetDeviceQueue(IntPtr dev, uint family, uint index, out IntPtr q);
        [DllImport(Dll)] static extern int vkCreateBuffer(IntPtr dev, void* ci, void* alloc, out ulong buf);
        [DllImport(Dll)] static extern void vkDestroyBuffer(IntPtr dev, ulong buf, void* alloc);
        [DllImport(Dll)] static extern void vkGetBufferMemoryRequirements(IntPtr dev, ulong buf, ulong* req);
        [DllImport(Dll)] static extern int vkAllocateMemory(IntPtr dev, void* ai, void* alloc, out ulong mem);
        [DllImport(Dll)] static extern void vkFreeMemory(IntPtr dev, ulong mem, void* alloc);
        [DllImport(Dll)] static extern int vkBindBufferMemory(IntPtr dev, ulong buf, ulong mem, ulong off);
        [DllImport(Dll)] static extern int vkMapMemory(IntPtr dev, ulong mem, ulong off, ulong size, uint flags, out IntPtr data);
        [DllImport(Dll)] static extern int vkCreateDescriptorSetLayout(IntPtr dev, void* ci, void* alloc, out ulong l);
        [DllImport(Dll)] static extern void vkDestroyDescriptorSetLayout(IntPtr dev, ulong l, void* alloc);
        [DllImport(Dll)] static extern int vkCreatePipelineLayout(IntPtr dev, void* ci, void* alloc, out ulong l);
        [DllImport(Dll)] static extern void vkDestroyPipelineLayout(IntPtr dev, ulong l, void* alloc);
        [DllImport(Dll)] static extern int vkCreateShaderModule(IntPtr dev, void* ci, void* alloc, out ulong m);
        [DllImport(Dll)] static extern void vkDestroyShaderModule(IntPtr dev, ulong m, void* alloc);
        [DllImport(Dll)] static extern int vkCreateComputePipelines(IntPtr dev, ulong cache, uint n, void* ci, void* alloc, out ulong p);
        [DllImport(Dll)] static extern void vkDestroyPipeline(IntPtr dev, ulong p, void* alloc);
        [DllImport(Dll)] static extern int vkCreateDescriptorPool(IntPtr dev, void* ci, void* alloc, out ulong p);
        [DllImport(Dll)] static extern void vkDestroyDescriptorPool(IntPtr dev, ulong p, void* alloc);
        [DllImport(Dll)] static extern int vkAllocateDescriptorSets(IntPtr dev, void* ai, ulong* sets);
        [DllImport(Dll)] static extern void vkUpdateDescriptorSets(IntPtr dev, uint n, void* writes, uint c, void* copies);
        [DllImport(Dll)] static extern int vkCreateCommandPool(IntPtr dev, void* ci, void* alloc, out ulong p);
        [DllImport(Dll)] static extern void vkDestroyCommandPool(IntPtr dev, ulong p, void* alloc);
        [DllImport(Dll)] static extern int vkAllocateCommandBuffers(IntPtr dev, void* ai, out IntPtr cb);
        [DllImport(Dll)] static extern int vkBeginCommandBuffer(IntPtr cb, void* bi);
        [DllImport(Dll)] static extern int vkEndCommandBuffer(IntPtr cb);
        [DllImport(Dll)] static extern int vkResetCommandBuffer(IntPtr cb, uint flags);
        [DllImport(Dll)] static extern void vkCmdBindPipeline(IntPtr cb, uint bp, ulong p);
        [DllImport(Dll)] static extern void vkCmdBindDescriptorSets(IntPtr cb, uint bp, ulong layout, uint first, uint n, ulong* sets, uint nd, uint* dyn);
        [DllImport(Dll)] static extern void vkCmdDispatch(IntPtr cb, uint x, uint y, uint z);
        [DllImport(Dll)] static extern void vkCmdPipelineBarrier(IntPtr cb, uint src, uint dst, uint dep, uint nm, void* mb, uint nb, void* bb, uint ni, void* ib);
        [DllImport(Dll)] static extern int vkQueueSubmit(IntPtr q, uint n, void* submits, ulong fence);
        [DllImport(Dll)] static extern int vkQueueWaitIdle(IntPtr q);
        [DllImport(Dll)] static extern int vkDeviceWaitIdle(IntPtr dev);
        [DllImport(Dll)] static extern IntPtr vkGetInstanceProcAddr(IntPtr inst, string name);
        [DllImport(Dll)] static extern IntPtr vkGetDeviceProcAddr(IntPtr dev, string name);
        [DllImport(Dll)] static extern void vkGetPhysicalDeviceProperties2(IntPtr pd, byte* props);

        IntPtr _inst, _pd, _dev, _queue, _cb;
        ulong _ssbo, _ssboMem, _ubo, _uboMem, _set0Layout, _set1Layout, _pipeLayout, _pool, _cmdPool;
        ulong _set0, _set1;
        IntPtr _ssboMap;
        readonly int _bytes;
        public string DeviceName { get; private set; }

        static void Check(int r, string what) { if (r != 0) throw new Exception($"{what}: VkResult {r}"); }

        static byte* Z(int n) { byte* p = (byte*)NativeMemory.AllocZeroed((nuint)n); return p; }
        static void U32(byte* p, int off, uint v) => *(uint*)(p + off) = v;
        static void U64(byte* p, int off, ulong v) => *(ulong*)(p + off) = v;
        static void Ptr(byte* p, int off, void* v) => *(void**)(p + off) = v;

        /// VK_KHR_cooperative_matrix properties (M, N, K, A, B, C, result type, saturating, scope) when made with coopMat.
        public readonly System.Collections.Generic.List<(uint m, uint n, uint k, uint a, uint b, uint c, uint r, uint sat, uint scope)> CoopMatProps = new();
        public uint SubgroupSize { get; private set; }

        public VkRunner(int bytes) : this(bytes, false) { }

        /// coopMat: also enables VK_KHR_cooperative_matrix and reads its properties (CoopMat.cs).
        public VkRunner(int bytes, bool coopMat)
        {
            _bytes = bytes;
            byte* app = Z(48);
            U32(app, 0, 0); // VK_STRUCTURE_TYPE_APPLICATION_INFO
            U32(app, 44, (1u << 22) | (3u << 12));   // apiVersion 1.3
            byte* ici = Z(64);
            U32(ici, 0, 1);  // INSTANCE_CREATE_INFO
            Ptr(ici, 24, app);
            Check(vkCreateInstance(ici, null, out _inst), "vkCreateInstance");

            uint n = 0;
            vkEnumeratePhysicalDevices(_inst, ref n, null);
            IntPtr* pds = stackalloc IntPtr[(int)n];
            vkEnumeratePhysicalDevices(_inst, ref n, pds);
            byte* props = Z(2048);
            for (int i = 0; i < n; i++)
            {
                vkGetPhysicalDeviceProperties(pds[i], props);
                if (*(uint*)(props + 8) == 0x10de) { _pd = pds[i]; break; }   // NVIDIA
            }
            if (_pd == IntPtr.Zero) throw new Exception("no NVIDIA Vulkan device");
            DeviceName = Marshal.PtrToStringAnsi((IntPtr)(props + 20));

            uint qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(_pd, ref qn, null);
            byte* qp = Z((int)qn * 24);
            vkGetPhysicalDeviceQueueFamilyProperties(_pd, ref qn, qp);
            uint family = uint.MaxValue;
            for (uint i = 0; i < qn; i++) if ((*(uint*)(qp + i * 24) & 2) != 0) { family = i; break; }   // COMPUTE

            // features: query everything, enable everything
            byte* f2 = Z(512), f11 = Z(512), f12 = Z(512), f13 = Z(512);
            U32(f2, 0, 1000059000); Ptr(f2, 8, f11);
            U32(f11, 0, 49); Ptr(f11, 8, f12);
            U32(f12, 0, 51); Ptr(f12, 8, f13);
            U32(f13, 0, 53);
            byte* fcm = Z(32);
            byte* fpe = Z(32);
            if (coopMat)
            {
                U32(fcm, 0, 1000506000); Ptr(f13, 8, fcm);   // VkPhysicalDeviceCooperativeMatrixFeaturesKHR
                U32(fpe, 0, 1000269000); Ptr(fcm, 8, fpe);   // VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR (PipelineStats)
            }
            vkGetPhysicalDeviceFeatures2(_pd, f2);

            float prio = 1;
            byte* qci = Z(40);
            U32(qci, 0, 2); U32(qci, 20, family); U32(qci, 24, 1); Ptr(qci, 32, &prio);
            byte* dci = Z(72);
            U32(dci, 0, 3); Ptr(dci, 8, f2); U32(dci, 20, 1); Ptr(dci, 24, qci);
            if (coopMat)
            {
                if (*(uint*)(fcm + 16) == 0) throw new Exception("cooperativeMatrix feature not supported");
                byte* extName = Z(64);
                byte[] en = Encoding.ASCII.GetBytes("VK_KHR_cooperative_matrix");
                for (int i = 0; i < en.Length; i++) extName[i] = en[i];
                byte* extName2 = Z(64);
                byte[] en2 = Encoding.ASCII.GetBytes("VK_KHR_pipeline_executable_properties");
                for (int i = 0; i < en2.Length; i++) extName2[i] = en2[i];
                byte** extNames = (byte**)Z(16);
                extNames[0] = extName;
                extNames[1] = extName2;
                U32(dci, 48, 2); Ptr(dci, 56, extNames);
                var getProps = (delegate* unmanaged<IntPtr, uint*, byte*, int>)vkGetInstanceProcAddr(_inst, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
                if (getProps == null) throw new Exception("no vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
                uint pn = 0;
                getProps(_pd, &pn, null);
                byte* pp = Z((int)pn * 56 + 8);
                for (uint i = 0; i < pn; i++) U32(pp + i * 56, 0, 1000506001);
                getProps(_pd, &pn, pp);
                for (uint i = 0; i < pn; i++)
                {
                    uint* q = (uint*)(pp + i * 56 + 16);
                    CoopMatProps.Add((q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8]));
                }
                byte* sp = Z(64), p2 = Z(1024);
                U32(sp, 0, 1000094000);   // VkPhysicalDeviceSubgroupProperties
                U32(p2, 0, 1000059001); Ptr(p2, 8, sp);
                vkGetPhysicalDeviceProperties2(_pd, p2);
                SubgroupSize = *(uint*)(sp + 16);
            }
            Check(vkCreateDevice(_pd, dci, null, out _dev), "vkCreateDevice");
            vkGetDeviceQueue(_dev, family, 0, out _queue);

            byte* mp = Z(1024);
            vkGetPhysicalDeviceMemoryProperties(_pd, mp);
            (_ssbo, _ssboMem) = Buffer((ulong)bytes, 0x20 /* STORAGE */, mp);
            (_ubo, _uboMem) = Buffer(0x10000, 0x10 /* UNIFORM */, mp);
            Check(vkMapMemory(_dev, _ssboMem, 0, ulong.MaxValue, 0, out _ssboMap), "vkMapMemory");
            Check(vkMapMemory(_dev, _uboMem, 0, ulong.MaxValue, 0, out IntPtr uboMap), "vkMapMemory");
            new Span<byte>((void*)uboMap, 0x10000).Clear();
            // support buffer (Ryujinx's render-scale and alpha-test data): declared by every module, left zero

            _set0Layout = SetLayout(6 /* UNIFORM_BUFFER */);
            _set1Layout = SetLayout(7 /* STORAGE_BUFFER */);
            ulong* layouts = stackalloc ulong[2] { _set0Layout, _set1Layout };
            byte* plci = Z(48);
            U32(plci, 0, 30); U32(plci, 20, 2); Ptr(plci, 24, layouts);
            Check(vkCreatePipelineLayout(_dev, plci, null, out _pipeLayout), "vkCreatePipelineLayout");

            uint* sizes = stackalloc uint[4] { 6, 1, 7, 1 };
            byte* dpci = Z(40);
            U32(dpci, 0, 33); U32(dpci, 20, 2); U32(dpci, 24, 2); Ptr(dpci, 32, sizes);
            Check(vkCreateDescriptorPool(_dev, dpci, null, out _pool), "vkCreateDescriptorPool");
            byte* dsai = Z(40);
            U32(dsai, 0, 34); U64(dsai, 16, _pool); U32(dsai, 24, 2); Ptr(dsai, 32, layouts);
            ulong* sets = stackalloc ulong[2];
            Check(vkAllocateDescriptorSets(_dev, dsai, sets), "vkAllocateDescriptorSets");
            _set0 = sets[0]; _set1 = sets[1];

            ulong* bi = stackalloc ulong[6] { _ubo, 0, ulong.MaxValue, _ssbo, 0, ulong.MaxValue };
            byte* w = Z(128);
            U32(w, 0, 35); U64(w, 16, _set0); U32(w, 32, 1); U32(w, 36, 6); Ptr(w, 48, bi);
            U32(w + 64, 0, 35); U64(w + 64, 16, _set1); U32(w + 64, 32, 1); U32(w + 64, 36, 7); Ptr(w + 64, 48, bi + 3);
            vkUpdateDescriptorSets(_dev, 2, w, 0, null);

            byte* cpci = Z(24);
            U32(cpci, 0, 39); U32(cpci, 16, 2 /* RESET_COMMAND_BUFFER */); U32(cpci, 20, family);
            Check(vkCreateCommandPool(_dev, cpci, null, out _cmdPool), "vkCreateCommandPool");
            byte* cbai = Z(32);
            U32(cbai, 0, 40); U64(cbai, 16, _cmdPool); U32(cbai, 24, 0); U32(cbai, 28, 1);
            Check(vkAllocateCommandBuffers(_dev, cbai, out _cb), "vkAllocateCommandBuffers");
        }

        ulong SetLayout(uint type)
        {
            byte* b = Z(24);
            U32(b, 0, 0); U32(b, 4, type); U32(b, 8, 1); U32(b, 12, 0x20 /* COMPUTE */);
            byte* ci = Z(32);
            U32(ci, 0, 32); U32(ci, 20, 1); Ptr(ci, 24, b);
            Check(vkCreateDescriptorSetLayout(_dev, ci, null, out ulong l), "vkCreateDescriptorSetLayout");
            return l;
        }

        (ulong, ulong) Buffer(ulong size, uint usage, byte* mp)
        {
            byte* bci = Z(56);
            U32(bci, 0, 12); U64(bci, 24, size); U32(bci, 32, usage);
            Check(vkCreateBuffer(_dev, bci, null, out ulong buf), "vkCreateBuffer");
            ulong* req = stackalloc ulong[3];
            vkGetBufferMemoryRequirements(_dev, buf, req);
            uint bits = (uint)req[2], count = *(uint*)mp, type = uint.MaxValue;
            for (uint i = 0; i < count; i++)
            {
                uint flags = *(uint*)(mp + 4 + i * 8);
                if ((bits & (1u << (int)i)) != 0 && (flags & 6) == 6) { type = i; break; }   // HOST_VISIBLE | HOST_COHERENT
            }
            byte* mai = Z(32);
            U32(mai, 0, 5); U64(mai, 16, req[0]); U32(mai, 24, type);
            Check(vkAllocateMemory(_dev, mai, null, out ulong mem), "vkAllocateMemory");
            Check(vkBindBufferMemory(_dev, buf, mem, 0), "vkBindBufferMemory");
            return (buf, mem);
        }

        /// Runs one module over `data` (in place) with groups x 32 invocations. Returns null or an error text.
        public string Run(byte[] spirv, uint[] data, int groups)
        {
            ulong module = 0, pipe = 0;
            try
            {
                fixed (byte* code = spirv)
                {
                    byte* smci = Z(40);
                    U32(smci, 0, 16); U64(smci, 24, (ulong)spirv.Length); Ptr(smci, 32, code);
                    int r = vkCreateShaderModule(_dev, smci, null, out module);
                    if (r != 0) return $"vkCreateShaderModule {r}";
                }
                byte* name = Z(8);
                name[0] = (byte)'m'; name[1] = (byte)'a'; name[2] = (byte)'i'; name[3] = (byte)'n';
                byte* cpci = Z(96);
                U32(cpci, 0, 29);
                U32(cpci, 24, 18); U32(cpci, 24 + 20, 0x20); U64(cpci, 24 + 24, module); Ptr(cpci, 24 + 32, name);
                U64(cpci, 72, _pipeLayout);
                *(int*)(cpci + 88) = -1;
                int pr = vkCreateComputePipelines(_dev, 0, 1, cpci, null, out pipe);
                if (pr != 0) return $"vkCreateComputePipelines {pr}";

                fixed (uint* d = data) System.Buffer.MemoryCopy(d, (void*)_ssboMap, _bytes, _bytes);
                Check(vkResetCommandBuffer(_cb, 0), "vkResetCommandBuffer");
                byte* cbbi = Z(32);
                U32(cbbi, 0, 42); U32(cbbi, 16, 1 /* ONE_TIME_SUBMIT */);
                Check(vkBeginCommandBuffer(_cb, cbbi), "vkBeginCommandBuffer");
                vkCmdBindPipeline(_cb, 1, pipe);
                ulong* sets = stackalloc ulong[2] { _set0, _set1 };
                vkCmdBindDescriptorSets(_cb, 1, _pipeLayout, 0, 2, sets, 0, null);
                vkCmdDispatch(_cb, (uint)groups, 1, 1);
                byte* mb = Z(24);
                U32(mb, 0, 46); U32(mb, 16, 0x40 /* SHADER_WRITE */); U32(mb, 20, 0x2000 /* HOST_READ */);
                vkCmdPipelineBarrier(_cb, 0x800 /* COMPUTE */, 0x4000 /* HOST */, 0, 1, mb, 0, null, 0, null);
                Check(vkEndCommandBuffer(_cb), "vkEndCommandBuffer");
                IntPtr cb = _cb;
                byte* si = Z(72);
                U32(si, 0, 4); U32(si, 40, 1); Ptr(si, 48, &cb);
                int sr = vkQueueSubmit(_queue, 1, si, 0);
                if (sr != 0) return $"vkQueueSubmit {sr}";
                int wr = vkQueueWaitIdle(_queue);
                if (wr != 0) return $"vkQueueWaitIdle {wr}";
                fixed (uint* d = data) System.Buffer.MemoryCopy((void*)_ssboMap, d, _bytes, _bytes);
                return null;
            }
            finally
            {
                if (pipe != 0) vkDestroyPipeline(_dev, pipe, null);
                if (module != 0) vkDestroyShaderModule(_dev, module, null);
            }
        }

        /// The driver's statistics of a compute module (VK_KHR_pipeline_executable_properties; made with coopMat). `types`: the
        /// descriptor types of binding 0.. of each set (one array per set), as the module declares them.
        public System.Collections.Generic.List<string> PipelineStats(byte[] spirv, uint[][] types)
        {
            var res = new System.Collections.Generic.List<string>();
            ulong module = 0, pipe = 0, layout = 0;
            var setLayouts = new ulong[types.Length];
            try
            {
                for (int s = 0; s < types.Length; s++)
                {
                    byte* b = Z(24 * Math.Max(1, types[s].Length));
                    for (int i = 0; i < types[s].Length; i++) { U32(b + 24 * i, 0, (uint)i); U32(b + 24 * i, 4, types[s][i]); U32(b + 24 * i, 8, 1); U32(b + 24 * i, 12, 0x20); }
                    byte* ci = Z(32);
                    U32(ci, 0, 32); U32(ci, 20, (uint)types[s].Length); Ptr(ci, 24, b);
                    Check(vkCreateDescriptorSetLayout(_dev, ci, null, out setLayouts[s]), "set layout");
                }
                fixed (ulong* sl = setLayouts)
                {
                    byte* plci = Z(48);
                    U32(plci, 0, 30); U32(plci, 20, (uint)types.Length); Ptr(plci, 24, sl);
                    Check(vkCreatePipelineLayout(_dev, plci, null, out layout), "pipeline layout");
                }
                fixed (byte* code = spirv)
                {
                    byte* smci = Z(40);
                    U32(smci, 0, 16); U64(smci, 24, (ulong)spirv.Length); Ptr(smci, 32, code);
                    Check(vkCreateShaderModule(_dev, smci, null, out module), "vkCreateShaderModule");
                }
                byte* name = Z(8);
                name[0] = (byte)'m'; name[1] = (byte)'a'; name[2] = (byte)'i'; name[3] = (byte)'n';
                byte* cpci = Z(96);
                U32(cpci, 0, 29); U32(cpci, 16, 0x40 /* CAPTURE_STATISTICS */);
                U32(cpci, 24, 18); U32(cpci, 24 + 20, 0x20); U64(cpci, 24 + 24, module); Ptr(cpci, 24 + 32, name);
                U64(cpci, 72, layout);
                *(int*)(cpci + 88) = -1;
                var sw = System.Diagnostics.Stopwatch.StartNew();
                Check(vkCreateComputePipelines(_dev, 0, 1, cpci, null, out pipe), "vkCreateComputePipelines");
                res.Add($"pipeline built in {sw.ElapsedMilliseconds} ms");
                var getExe = (delegate* unmanaged<IntPtr, byte*, uint*, byte*, int>)vkGetDeviceProcAddr(_dev, "vkGetPipelineExecutablePropertiesKHR");
                var getStat = (delegate* unmanaged<IntPtr, byte*, uint*, byte*, int>)vkGetDeviceProcAddr(_dev, "vkGetPipelineExecutableStatisticsKHR");
                byte* pi = Z(24);
                U32(pi, 0, 1000269001); U64(pi, 16, pipe);
                uint ne = 0;
                getExe(_dev, pi, &ne, null);
                byte* ep = Z((int)ne * 536 + 8);
                for (uint i = 0; i < ne; i++) U32(ep + i * 536, 0, 1000269002);
                getExe(_dev, pi, &ne, ep);
                for (uint e = 0; e < ne; e++)
                {
                    res.Add($"executable {e}: {Marshal.PtrToStringAnsi((IntPtr)(ep + e * 536 + 20))}, subgroup size {*(uint*)(ep + e * 536 + 532)}");
                    byte* ei = Z(32);
                    U32(ei, 0, 1000269003); U64(ei, 16, pipe); U32(ei, 24, e);
                    uint ns = 0;
                    getStat(_dev, ei, &ns, null);
                    byte* st = Z((int)ns * 544 + 8);
                    for (uint i = 0; i < ns; i++) U32(st + i * 544, 0, 1000269004);
                    getStat(_dev, ei, &ns, st);
                    for (uint i = 0; i < ns; i++)
                    {
                        byte* x = st + i * 544;
                        uint fmt = *(uint*)(x + 528);
                        string v = fmt switch { 0 => (*(uint*)(x + 536) != 0).ToString(), 1 => (*(long*)(x + 536)).ToString(), 2 => (*(ulong*)(x + 536)).ToString(), _ => (*(double*)(x + 536)).ToString("0.###") };
                        res.Add($"  {Marshal.PtrToStringAnsi((IntPtr)(x + 16))} = {v}");
                    }
                }
            }
            catch (Exception ex) { res.Add("error: " + ex.Message); }
            finally
            {
                if (pipe != 0) vkDestroyPipeline(_dev, pipe, null);
                if (module != 0) vkDestroyShaderModule(_dev, module, null);
                if (layout != 0) vkDestroyPipelineLayout(_dev, layout, null);
                foreach (ulong l in setLayouts) if (l != 0) vkDestroyDescriptorSetLayout(_dev, l, null);
            }
            return res;
        }

        public void Dispose()
        {
            if (_dev == IntPtr.Zero) return;
            vkDeviceWaitIdle(_dev);
            vkDestroyCommandPool(_dev, _cmdPool, null);
            vkDestroyDescriptorPool(_dev, _pool, null);
            vkDestroyPipelineLayout(_dev, _pipeLayout, null);
            vkDestroyDescriptorSetLayout(_dev, _set0Layout, null);
            vkDestroyDescriptorSetLayout(_dev, _set1Layout, null);
            vkDestroyBuffer(_dev, _ssbo, null); vkFreeMemory(_dev, _ssboMem, null);
            vkDestroyBuffer(_dev, _ubo, null); vkFreeMemory(_dev, _uboMem, null);
            vkDestroyDevice(_dev, null);
            vkDestroyInstance(_inst, null);
        }
    }
}
