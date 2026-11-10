// SPDX-License-Identifier: GPL-3.0-or-later
// NVNformat -> FormatInfo table and the vertex-attribute format map (S4d). Interface: nvn_format.h.
// Sources read to fix the rows (no code copied):
//   refs/smo-recomp/runtime/gfx/nvn_gl.c: fmt_get() and its k_* tables (byte size per texel or block, block footprint,
//     colour/depth/stencil kind, the GL REV layouts of the packed 16 and 32-bit formats) and vfmt_get() (vertex
//     formats, UI2F/I2F = scaled, norm and integer flags).
//   refs/SuperMarioWonderRecomp/runtime/src/nvn/nvn_format.cpp: NVN value -> VkFormat (the 0x00..0x9C order, the ASTC
//     and BGR families, DEPTH24 -> D32, R11G11B10F fallback to R16G16B16A16_SFLOAT).
//   refs/SuperMarioWonderRecomp/docs/nvn/TILING.md: block-linear storage counts in blocks of bytes_per_block.
//   the texture formats games use; tests/gfx/test_nvn_format.cpp checks them.
// Where smo and Wonder disagree on a Vulkan layout, the row carries a comment saying which one was used.
// Row i is the NVNformat value i + 1 (NONE is 0, LARGE is a sentinel and has no row).

#include "nvn_format.h"

namespace nxrt::nvn {

namespace {

// Columns: name, nvn, bytes, block width, block height, aspect, channels, srgb, compressed, astc, bc, is_float,
// is_snorm, is_int, is_uint, vk, vk_fallback.
constexpr FormatInfo kTable[] = {
    // R, RG, RGB, RGBA families (NVN values 0x01..0x30)
    {"R8", NVN_FORMAT_R8, 1, 1, 1, kAspectColor, 1, false, false, false, false, false, false, false, false,
     VK_FORMAT_R8_UNORM, VK_FORMAT_UNDEFINED},
    {"R8SN", NVN_FORMAT_R8SN, 1, 1, 1, kAspectColor, 1, false, false, false, false, false, true, false, false,
     VK_FORMAT_R8_SNORM, VK_FORMAT_UNDEFINED},
    {"R8UI", NVN_FORMAT_R8UI, 1, 1, 1, kAspectColor, 1, false, false, false, false, false, false, false, true,
     VK_FORMAT_R8_UINT, VK_FORMAT_UNDEFINED},
    {"R8I", NVN_FORMAT_R8I, 1, 1, 1, kAspectColor, 1, false, false, false, false, false, false, true, false,
     VK_FORMAT_R8_SINT, VK_FORMAT_UNDEFINED},
    {"R16F", NVN_FORMAT_R16F, 2, 1, 1, kAspectColor, 1, false, false, false, false, true, false, false, false,
     VK_FORMAT_R16_SFLOAT, VK_FORMAT_UNDEFINED},
    {"R16", NVN_FORMAT_R16, 2, 1, 1, kAspectColor, 1, false, false, false, false, false, false, false, false,
     VK_FORMAT_R16_UNORM, VK_FORMAT_UNDEFINED},
    {"R16SN", NVN_FORMAT_R16SN, 2, 1, 1, kAspectColor, 1, false, false, false, false, false, true, false, false,
     VK_FORMAT_R16_SNORM, VK_FORMAT_UNDEFINED},
    {"R16UI", NVN_FORMAT_R16UI, 2, 1, 1, kAspectColor, 1, false, false, false, false, false, false, false, true,
     VK_FORMAT_R16_UINT, VK_FORMAT_UNDEFINED},
    {"R16I", NVN_FORMAT_R16I, 2, 1, 1, kAspectColor, 1, false, false, false, false, false, false, true, false,
     VK_FORMAT_R16_SINT, VK_FORMAT_UNDEFINED},
    {"R32F", NVN_FORMAT_R32F, 4, 1, 1, kAspectColor, 1, false, false, false, false, true, false, false, false,
     VK_FORMAT_R32_SFLOAT, VK_FORMAT_UNDEFINED},
    {"R32UI", NVN_FORMAT_R32UI, 4, 1, 1, kAspectColor, 1, false, false, false, false, false, false, false, true,
     VK_FORMAT_R32_UINT, VK_FORMAT_UNDEFINED},
    {"R32I", NVN_FORMAT_R32I, 4, 1, 1, kAspectColor, 1, false, false, false, false, false, false, true, false,
     VK_FORMAT_R32_SINT, VK_FORMAT_UNDEFINED},
    {"RG8", NVN_FORMAT_RG8, 2, 1, 1, kAspectColor, 2, false, false, false, false, false, false, false, false,
     VK_FORMAT_R8G8_UNORM, VK_FORMAT_UNDEFINED},
    {"RG8SN", NVN_FORMAT_RG8SN, 2, 1, 1, kAspectColor, 2, false, false, false, false, false, true, false, false,
     VK_FORMAT_R8G8_SNORM, VK_FORMAT_UNDEFINED},
    {"RG8UI", NVN_FORMAT_RG8UI, 2, 1, 1, kAspectColor, 2, false, false, false, false, false, false, false, true,
     VK_FORMAT_R8G8_UINT, VK_FORMAT_UNDEFINED},
    {"RG8I", NVN_FORMAT_RG8I, 2, 1, 1, kAspectColor, 2, false, false, false, false, false, false, true, false,
     VK_FORMAT_R8G8_SINT, VK_FORMAT_UNDEFINED},
    {"RG16F", NVN_FORMAT_RG16F, 4, 1, 1, kAspectColor, 2, false, false, false, false, true, false, false, false,
     VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_UNDEFINED},
    {"RG16", NVN_FORMAT_RG16, 4, 1, 1, kAspectColor, 2, false, false, false, false, false, false, false, false,
     VK_FORMAT_R16G16_UNORM, VK_FORMAT_UNDEFINED},
    {"RG16SN", NVN_FORMAT_RG16SN, 4, 1, 1, kAspectColor, 2, false, false, false, false, false, true, false, false,
     VK_FORMAT_R16G16_SNORM, VK_FORMAT_UNDEFINED},
    {"RG16UI", NVN_FORMAT_RG16UI, 4, 1, 1, kAspectColor, 2, false, false, false, false, false, false, false, true,
     VK_FORMAT_R16G16_UINT, VK_FORMAT_UNDEFINED},
    {"RG16I", NVN_FORMAT_RG16I, 4, 1, 1, kAspectColor, 2, false, false, false, false, false, false, true, false,
     VK_FORMAT_R16G16_SINT, VK_FORMAT_UNDEFINED},
    {"RG32F", NVN_FORMAT_RG32F, 8, 1, 1, kAspectColor, 2, false, false, false, false, true, false, false, false,
     VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_UNDEFINED},
    {"RG32UI", NVN_FORMAT_RG32UI, 8, 1, 1, kAspectColor, 2, false, false, false, false, false, false, false, true,
     VK_FORMAT_R32G32_UINT, VK_FORMAT_UNDEFINED},
    {"RG32I", NVN_FORMAT_RG32I, 8, 1, 1, kAspectColor, 2, false, false, false, false, false, false, true, false,
     VK_FORMAT_R32G32_SINT, VK_FORMAT_UNDEFINED},
    {"RGB8", NVN_FORMAT_RGB8, 3, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, false,
     VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_UNDEFINED},
    {"RGB8SN", NVN_FORMAT_RGB8SN, 3, 1, 1, kAspectColor, 3, false, false, false, false, false, true, false, false,
     VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_UNDEFINED},
    {"RGB8UI", NVN_FORMAT_RGB8UI, 3, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, true,
     VK_FORMAT_R8G8B8_UINT, VK_FORMAT_UNDEFINED},
    {"RGB8I", NVN_FORMAT_RGB8I, 3, 1, 1, kAspectColor, 3, false, false, false, false, false, false, true, false,
     VK_FORMAT_R8G8B8_SINT, VK_FORMAT_UNDEFINED},
    {"RGB16F", NVN_FORMAT_RGB16F, 6, 1, 1, kAspectColor, 3, false, false, false, false, true, false, false, false,
     VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_UNDEFINED},
    {"RGB16", NVN_FORMAT_RGB16, 6, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, false,
     VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_UNDEFINED},
    {"RGB16SN", NVN_FORMAT_RGB16SN, 6, 1, 1, kAspectColor, 3, false, false, false, false, false, true, false, false,
     VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_UNDEFINED},
    {"RGB16UI", NVN_FORMAT_RGB16UI, 6, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, true,
     VK_FORMAT_R16G16B16_UINT, VK_FORMAT_UNDEFINED},
    {"RGB16I", NVN_FORMAT_RGB16I, 6, 1, 1, kAspectColor, 3, false, false, false, false, false, false, true, false,
     VK_FORMAT_R16G16B16_SINT, VK_FORMAT_UNDEFINED},
    {"RGB32F", NVN_FORMAT_RGB32F, 12, 1, 1, kAspectColor, 3, false, false, false, false, true, false, false, false,
     VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_UNDEFINED},
    {"RGB32UI", NVN_FORMAT_RGB32UI, 12, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, true,
     VK_FORMAT_R32G32B32_UINT, VK_FORMAT_UNDEFINED},
    {"RGB32I", NVN_FORMAT_RGB32I, 12, 1, 1, kAspectColor, 3, false, false, false, false, false, false, true, false,
     VK_FORMAT_R32G32B32_SINT, VK_FORMAT_UNDEFINED},
    {"RGBA8", NVN_FORMAT_RGBA8, 4, 1, 1, kAspectColor, 4, false, false, false, false, false, false, false, false,
     VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED},
    {"RGBA8SN", NVN_FORMAT_RGBA8SN, 4, 1, 1, kAspectColor, 4, false, false, false, false, false, true, false, false,
     VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_UNDEFINED},
    {"RGBA8UI", NVN_FORMAT_RGBA8UI, 4, 1, 1, kAspectColor, 4, false, false, false, false, false, false, false, true,
     VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_UNDEFINED},
    {"RGBA8I", NVN_FORMAT_RGBA8I, 4, 1, 1, kAspectColor, 4, false, false, false, false, false, false, true, false,
     VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_UNDEFINED},
    {"RGBA16F", NVN_FORMAT_RGBA16F, 8, 1, 1, kAspectColor, 4, false, false, false, false, true, false, false, false,
     VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED},
    {"RGBA16", NVN_FORMAT_RGBA16, 8, 1, 1, kAspectColor, 4, false, false, false, false, false, false, false, false,
     VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_UNDEFINED},
    {"RGBA16SN", NVN_FORMAT_RGBA16SN, 8, 1, 1, kAspectColor, 4, false, false, false, false, false, true, false, false,
     VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_UNDEFINED},
    {"RGBA16UI", NVN_FORMAT_RGBA16UI, 8, 1, 1, kAspectColor, 4, false, false, false, false, false, false, false, true,
     VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_UNDEFINED},
    {"RGBA16I", NVN_FORMAT_RGBA16I, 8, 1, 1, kAspectColor, 4, false, false, false, false, false, false, true, false,
     VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_UNDEFINED},
    {"RGBA32F", NVN_FORMAT_RGBA32F, 16, 1, 1, kAspectColor, 4, false, false, false, false, true, false, false, false,
     VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_UNDEFINED},
    {"RGBA32UI", NVN_FORMAT_RGBA32UI, 16, 1, 1, kAspectColor, 4, false, false, false, false, false, false, false, true,
     VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_UNDEFINED},
    {"RGBA32I", NVN_FORMAT_RGBA32I, 16, 1, 1, kAspectColor, 4, false, false, false, false, false, false, true, false,
     VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_UNDEFINED},
    // Depth and stencil (0x31..0x36). DEPTH24 has no Vulkan equivalent on Apple M1 class GPUs: X8_D24 with D32 fallback.
    {"STENCIL8", NVN_FORMAT_STENCIL8, 1, 1, 1, kAspectStencil, 1, false, false, false, false, false, false, false, false,
     VK_FORMAT_S8_UINT, VK_FORMAT_UNDEFINED},
    {"DEPTH16", NVN_FORMAT_DEPTH16, 2, 1, 1, kAspectDepth, 1, false, false, false, false, false, false, false, false,
     VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT},
    {"DEPTH24", NVN_FORMAT_DEPTH24, 4, 1, 1, kAspectDepth, 1, false, false, false, false, false, false, false, false,
     VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT},
    {"DEPTH32F", NVN_FORMAT_DEPTH32F, 4, 1, 1, kAspectDepth, 1, false, false, false, false, true, false, false, false,
     VK_FORMAT_D32_SFLOAT, VK_FORMAT_UNDEFINED},
    {"DEPTH24_STENCIL8", NVN_FORMAT_DEPTH24_STENCIL8, 4, 1, 1, kAspectDepthStencil, 2, false, false, false, false, false, false, false, false,
     VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT},
    {"DEPTH32F_STENCIL8", NVN_FORMAT_DEPTH32F_STENCIL8, 8, 1, 1, kAspectDepthStencil, 2, false, false, false, false, true, false, false, false,
     VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_UNDEFINED},
    // sRGB RGBX8 and RGBA8 (0x37, 0x38). RGBX has three colour channels and an unused fourth.
    {"RGBX8_SRGB", NVN_FORMAT_RGBX8_SRGB, 4, 1, 1, kAspectColor, 3, true, false, false, false, false, false, false, false,
     VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_UNDEFINED},
    {"RGBA8_SRGB", NVN_FORMAT_RGBA8_SRGB, 4, 1, 1, kAspectColor, 4, true, false, false, false, false, false, false, false,
     VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_UNDEFINED},
    // Packed colour (0x39..0x40). RGB5 and RGB5A1 use the GL REV bit layout (R in the low bits, A in the top bit) of smo.
    {"RGBA4", NVN_FORMAT_RGBA4, 2, 1, 1, kAspectColor, 4, false, false, false, false, false, false, false, false,
     VK_FORMAT_A4B4G4R4_UNORM_PACK16, VK_FORMAT_UNDEFINED},
    // RGB5 has no alpha: bit 15 is unused. Wonder maps it to A1R5G5B5, which swaps R and B against smo's layout. Unverified on hardware.
    {"RGB5", NVN_FORMAT_RGB5, 2, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, false,
     VK_FORMAT_A1B5G5R5_UNORM_PACK16, VK_FORMAT_UNDEFINED},
    // Wonder maps RGB5A1 to A1R5G5B5 (R and B swapped against smo's REV layout). Unverified on hardware.
    {"RGB5A1", NVN_FORMAT_RGB5A1, 2, 1, 1, kAspectColor, 4, false, false, false, false, false, false, false, false,
     VK_FORMAT_A1B5G5R5_UNORM_PACK16, VK_FORMAT_UNDEFINED},
    {"RGB565", NVN_FORMAT_RGB565, 2, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, false,
     VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_UNDEFINED},
    {"RGB10A2", NVN_FORMAT_RGB10A2, 4, 1, 1, kAspectColor, 4, false, false, false, false, false, false, false, false,
     VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_UNDEFINED},
    {"RGB10A2UI", NVN_FORMAT_RGB10A2UI, 4, 1, 1, kAspectColor, 4, false, false, false, false, false, false, false, true,
     VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_UNDEFINED},
    // R11G11B10F is 4 bytes. Wonder stores it as R16G16B16A16_SFLOAT (8 bytes) when B10G11R11 is unavailable: that is the fallback.
    {"R11G11B10F", NVN_FORMAT_R11G11B10F, 4, 1, 1, kAspectColor, 3, false, false, false, false, true, false, false, false,
     VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R16G16B16A16_SFLOAT},
    {"RGB9E5F", NVN_FORMAT_RGB9E5F, 4, 1, 1, kAspectColor, 3, false, false, false, false, true, false, false, false,
     VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, VK_FORMAT_UNDEFINED},
    // Block compressed: DXT = BC1..BC3, RGTC = BC4/BC5, BPTC = BC6H/BC7 (4x4 blocks)
    {"RGB_DXT1", NVN_FORMAT_RGB_DXT1, 8, 4, 4, kAspectColor, 3, false, true, false, true, false, false, false, false,
     VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_DXT1", NVN_FORMAT_RGBA_DXT1, 8, 4, 4, kAspectColor, 4, false, true, false, true, false, false, false, false,
     VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_DXT3", NVN_FORMAT_RGBA_DXT3, 16, 4, 4, kAspectColor, 4, false, true, false, true, false, false, false, false,
     VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_DXT5", NVN_FORMAT_RGBA_DXT5, 16, 4, 4, kAspectColor, 4, false, true, false, true, false, false, false, false,
     VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGB_DXT1_SRGB", NVN_FORMAT_RGB_DXT1_SRGB, 8, 4, 4, kAspectColor, 3, true, true, false, true, false, false, false, false,
     VK_FORMAT_BC1_RGB_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_DXT1_SRGB", NVN_FORMAT_RGBA_DXT1_SRGB, 8, 4, 4, kAspectColor, 4, true, true, false, true, false, false, false, false,
     VK_FORMAT_BC1_RGBA_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_DXT3_SRGB", NVN_FORMAT_RGBA_DXT3_SRGB, 16, 4, 4, kAspectColor, 4, true, true, false, true, false, false, false, false,
     VK_FORMAT_BC2_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_DXT5_SRGB", NVN_FORMAT_RGBA_DXT5_SRGB, 16, 4, 4, kAspectColor, 4, true, true, false, true, false, false, false, false,
     VK_FORMAT_BC3_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGTC1_UNORM", NVN_FORMAT_RGTC1_UNORM, 8, 4, 4, kAspectColor, 1, false, true, false, true, false, false, false, false,
     VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGTC1_SNORM", NVN_FORMAT_RGTC1_SNORM, 8, 4, 4, kAspectColor, 1, false, true, false, true, false, true, false, false,
     VK_FORMAT_BC4_SNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGTC2_UNORM", NVN_FORMAT_RGTC2_UNORM, 16, 4, 4, kAspectColor, 2, false, true, false, true, false, false, false, false,
     VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGTC2_SNORM", NVN_FORMAT_RGTC2_SNORM, 16, 4, 4, kAspectColor, 2, false, true, false, true, false, true, false, false,
     VK_FORMAT_BC5_SNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"BPTC_UNORM", NVN_FORMAT_BPTC_UNORM, 16, 4, 4, kAspectColor, 4, false, true, false, true, false, false, false, false,
     VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"BPTC_UNORM_SRGB", NVN_FORMAT_BPTC_UNORM_SRGB, 16, 4, 4, kAspectColor, 4, true, true, false, true, false, false, false, false,
     VK_FORMAT_BC7_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
    {"BPTC_SFLOAT", NVN_FORMAT_BPTC_SFLOAT, 16, 4, 4, kAspectColor, 3, false, true, false, true, true, false, false, false,
     VK_FORMAT_BC6H_SFLOAT_BLOCK, VK_FORMAT_UNDEFINED},
    {"BPTC_UFLOAT", NVN_FORMAT_BPTC_UFLOAT, 16, 4, 4, kAspectColor, 3, false, true, false, true, true, false, false, false,
     VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_UNDEFINED},
    // Scaled forms (UI2F/I2F, 0x51..0x68): integer storage converted to float, not normalised. No 32-bit Vulkan scaled format.
    {"R8_UI2F", NVN_FORMAT_R8_UI2F, 1, 1, 1, kAspectColor, 1, false, false, false, false, false, false, false, false,
     VK_FORMAT_R8_USCALED, VK_FORMAT_UNDEFINED},
    {"R8_I2F", NVN_FORMAT_R8_I2F, 1, 1, 1, kAspectColor, 1, false, false, false, false, false, false, false, false,
     VK_FORMAT_R8_SSCALED, VK_FORMAT_UNDEFINED},
    {"R16_UI2F", NVN_FORMAT_R16_UI2F, 2, 1, 1, kAspectColor, 1, false, false, false, false, false, false, false, false,
     VK_FORMAT_R16_USCALED, VK_FORMAT_UNDEFINED},
    {"R16_I2F", NVN_FORMAT_R16_I2F, 2, 1, 1, kAspectColor, 1, false, false, false, false, false, false, false, false,
     VK_FORMAT_R16_SSCALED, VK_FORMAT_UNDEFINED},
    {"R32_UI2F", NVN_FORMAT_R32_UI2F, 4, 1, 1, kAspectColor, 1, false, false, false, false, false, false, false, false,
     VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED},
    {"R32_I2F", NVN_FORMAT_R32_I2F, 4, 1, 1, kAspectColor, 1, false, false, false, false, false, false, false, false,
     VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED},
    {"RG8_UI2F", NVN_FORMAT_RG8_UI2F, 2, 1, 1, kAspectColor, 2, false, false, false, false, false, false, false, false,
     VK_FORMAT_R8G8_USCALED, VK_FORMAT_UNDEFINED},
    {"RG8_I2F", NVN_FORMAT_RG8_I2F, 2, 1, 1, kAspectColor, 2, false, false, false, false, false, false, false, false,
     VK_FORMAT_R8G8_SSCALED, VK_FORMAT_UNDEFINED},
    {"RG16_UI2F", NVN_FORMAT_RG16_UI2F, 4, 1, 1, kAspectColor, 2, false, false, false, false, false, false, false, false,
     VK_FORMAT_R16G16_USCALED, VK_FORMAT_UNDEFINED},
    {"RG16_I2F", NVN_FORMAT_RG16_I2F, 4, 1, 1, kAspectColor, 2, false, false, false, false, false, false, false, false,
     VK_FORMAT_R16G16_SSCALED, VK_FORMAT_UNDEFINED},
    {"RG32_UI2F", NVN_FORMAT_RG32_UI2F, 8, 1, 1, kAspectColor, 2, false, false, false, false, false, false, false, false,
     VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED},
    {"RG32_I2F", NVN_FORMAT_RG32_I2F, 8, 1, 1, kAspectColor, 2, false, false, false, false, false, false, false, false,
     VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED},
    {"RGB8_UI2F", NVN_FORMAT_RGB8_UI2F, 3, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, false,
     VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_UNDEFINED},
    {"RGB8_I2F", NVN_FORMAT_RGB8_I2F, 3, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, false,
     VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_UNDEFINED},
    {"RGB16_UI2F", NVN_FORMAT_RGB16_UI2F, 6, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, false,
     VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_UNDEFINED},
    {"RGB16_I2F", NVN_FORMAT_RGB16_I2F, 6, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, false,
     VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_UNDEFINED},
    {"RGB32_UI2F", NVN_FORMAT_RGB32_UI2F, 12, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, false,
     VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED},
    {"RGB32_I2F", NVN_FORMAT_RGB32_I2F, 12, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, false,
     VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED},
    {"RGBA8_UI2F", NVN_FORMAT_RGBA8_UI2F, 4, 1, 1, kAspectColor, 4, false, false, false, false, false, false, false, false,
     VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_UNDEFINED},
    {"RGBA8_I2F", NVN_FORMAT_RGBA8_I2F, 4, 1, 1, kAspectColor, 4, false, false, false, false, false, false, false, false,
     VK_FORMAT_R8G8B8A8_SSCALED, VK_FORMAT_UNDEFINED},
    {"RGBA16_UI2F", NVN_FORMAT_RGBA16_UI2F, 8, 1, 1, kAspectColor, 4, false, false, false, false, false, false, false, false,
     VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_UNDEFINED},
    {"RGBA16_I2F", NVN_FORMAT_RGBA16_I2F, 8, 1, 1, kAspectColor, 4, false, false, false, false, false, false, false, false,
     VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_UNDEFINED},
    {"RGBA32_UI2F", NVN_FORMAT_RGBA32_UI2F, 16, 1, 1, kAspectColor, 4, false, false, false, false, false, false, false, false,
     VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED},
    {"RGBA32_I2F", NVN_FORMAT_RGBA32_I2F, 16, 1, 1, kAspectColor, 4, false, false, false, false, false, false, false, false,
     VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED},
    // RGB10A2 signed, integer and scaled forms (0x69..0x6C)
    {"RGB10A2SN", NVN_FORMAT_RGB10A2SN, 4, 1, 1, kAspectColor, 4, false, false, false, false, false, true, false, false,
     VK_FORMAT_A2B10G10R10_SNORM_PACK32, VK_FORMAT_UNDEFINED},
    {"RGB10A2I", NVN_FORMAT_RGB10A2I, 4, 1, 1, kAspectColor, 4, false, false, false, false, false, false, true, false,
     VK_FORMAT_A2B10G10R10_SINT_PACK32, VK_FORMAT_UNDEFINED},
    {"RGB10A2_UI2F", NVN_FORMAT_RGB10A2_UI2F, 4, 1, 1, kAspectColor, 4, false, false, false, false, false, false, false, false,
     VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_UNDEFINED},
    {"RGB10A2_I2F", NVN_FORMAT_RGB10A2_I2F, 4, 1, 1, kAspectColor, 4, false, false, false, false, false, false, false, false,
     VK_FORMAT_A2B10G10R10_SSCALED_PACK32, VK_FORMAT_UNDEFINED},
    // RGBX family (0x6D..0x78): four stored components, the fourth is unused (channels = 3)
    {"RGBX8", NVN_FORMAT_RGBX8, 4, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, false,
     VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED},
    {"RGBX8SN", NVN_FORMAT_RGBX8SN, 4, 1, 1, kAspectColor, 3, false, false, false, false, false, true, false, false,
     VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_UNDEFINED},
    {"RGBX8UI", NVN_FORMAT_RGBX8UI, 4, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, true,
     VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_UNDEFINED},
    {"RGBX8I", NVN_FORMAT_RGBX8I, 4, 1, 1, kAspectColor, 3, false, false, false, false, false, false, true, false,
     VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_UNDEFINED},
    {"RGBX16F", NVN_FORMAT_RGBX16F, 8, 1, 1, kAspectColor, 3, false, false, false, false, true, false, false, false,
     VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED},
    {"RGBX16", NVN_FORMAT_RGBX16, 8, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, false,
     VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_UNDEFINED},
    {"RGBX16SN", NVN_FORMAT_RGBX16SN, 8, 1, 1, kAspectColor, 3, false, false, false, false, false, true, false, false,
     VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_UNDEFINED},
    {"RGBX16UI", NVN_FORMAT_RGBX16UI, 8, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, true,
     VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_UNDEFINED},
    {"RGBX16I", NVN_FORMAT_RGBX16I, 8, 1, 1, kAspectColor, 3, false, false, false, false, false, false, true, false,
     VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_UNDEFINED},
    {"RGBX32F", NVN_FORMAT_RGBX32F, 16, 1, 1, kAspectColor, 3, false, false, false, false, true, false, false, false,
     VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_UNDEFINED},
    {"RGBX32UI", NVN_FORMAT_RGBX32UI, 16, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, true,
     VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_UNDEFINED},
    {"RGBX32I", NVN_FORMAT_RGBX32I, 16, 1, 1, kAspectColor, 3, false, false, false, false, false, false, true, false,
     VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_UNDEFINED},
    // ASTC LDR, linear (0x79..0x86): 16 bytes per block, footprint from the name
    {"RGBA_ASTC_4x4", NVN_FORMAT_RGBA_ASTC_4x4, 16, 4, 4, kAspectColor, 4, false, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_5x4", NVN_FORMAT_RGBA_ASTC_5x4, 16, 5, 4, kAspectColor, 4, false, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_5x4_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_5x5", NVN_FORMAT_RGBA_ASTC_5x5, 16, 5, 5, kAspectColor, 4, false, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_5x5_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_6x5", NVN_FORMAT_RGBA_ASTC_6x5, 16, 6, 5, kAspectColor, 4, false, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_6x5_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_6x6", NVN_FORMAT_RGBA_ASTC_6x6, 16, 6, 6, kAspectColor, 4, false, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_8x5", NVN_FORMAT_RGBA_ASTC_8x5, 16, 8, 5, kAspectColor, 4, false, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_8x5_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_8x6", NVN_FORMAT_RGBA_ASTC_8x6, 16, 8, 6, kAspectColor, 4, false, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_8x6_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_8x8", NVN_FORMAT_RGBA_ASTC_8x8, 16, 8, 8, kAspectColor, 4, false, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_8x8_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_10x5", NVN_FORMAT_RGBA_ASTC_10x5, 16, 10, 5, kAspectColor, 4, false, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_10x5_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_10x6", NVN_FORMAT_RGBA_ASTC_10x6, 16, 10, 6, kAspectColor, 4, false, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_10x6_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_10x8", NVN_FORMAT_RGBA_ASTC_10x8, 16, 10, 8, kAspectColor, 4, false, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_10x8_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_10x10", NVN_FORMAT_RGBA_ASTC_10x10, 16, 10, 10, kAspectColor, 4, false, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_10x10_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_12x10", NVN_FORMAT_RGBA_ASTC_12x10, 16, 12, 10, kAspectColor, 4, false, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_12x10_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_12x12", NVN_FORMAT_RGBA_ASTC_12x12, 16, 12, 12, kAspectColor, 4, false, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_12x12_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    // ASTC LDR, sRGB (0x87..0x94)
    {"RGBA_ASTC_4x4_SRGB", NVN_FORMAT_RGBA_ASTC_4x4_SRGB, 16, 4, 4, kAspectColor, 4, true, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_4x4_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_5x4_SRGB", NVN_FORMAT_RGBA_ASTC_5x4_SRGB, 16, 5, 4, kAspectColor, 4, true, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_5x4_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_5x5_SRGB", NVN_FORMAT_RGBA_ASTC_5x5_SRGB, 16, 5, 5, kAspectColor, 4, true, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_5x5_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_6x5_SRGB", NVN_FORMAT_RGBA_ASTC_6x5_SRGB, 16, 6, 5, kAspectColor, 4, true, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_6x5_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_6x6_SRGB", NVN_FORMAT_RGBA_ASTC_6x6_SRGB, 16, 6, 6, kAspectColor, 4, true, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_6x6_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_8x5_SRGB", NVN_FORMAT_RGBA_ASTC_8x5_SRGB, 16, 8, 5, kAspectColor, 4, true, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_8x5_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_8x6_SRGB", NVN_FORMAT_RGBA_ASTC_8x6_SRGB, 16, 8, 6, kAspectColor, 4, true, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_8x6_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_8x8_SRGB", NVN_FORMAT_RGBA_ASTC_8x8_SRGB, 16, 8, 8, kAspectColor, 4, true, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_8x8_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_10x5_SRGB", NVN_FORMAT_RGBA_ASTC_10x5_SRGB, 16, 10, 5, kAspectColor, 4, true, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_10x5_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_10x6_SRGB", NVN_FORMAT_RGBA_ASTC_10x6_SRGB, 16, 10, 6, kAspectColor, 4, true, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_10x6_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_10x8_SRGB", NVN_FORMAT_RGBA_ASTC_10x8_SRGB, 16, 10, 8, kAspectColor, 4, true, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_10x8_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_10x10_SRGB", NVN_FORMAT_RGBA_ASTC_10x10_SRGB, 16, 10, 10, kAspectColor, 4, true, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_10x10_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_12x10_SRGB", NVN_FORMAT_RGBA_ASTC_12x10_SRGB, 16, 12, 10, kAspectColor, 4, true, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_12x10_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
    {"RGBA_ASTC_12x12_SRGB", NVN_FORMAT_RGBA_ASTC_12x12_SRGB, 16, 12, 12, kAspectColor, 4, true, true, true, false, false, false, false, false,
     VK_FORMAT_ASTC_12x12_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
    // BGR orders (0x95..0x9C). BGR5 has no alpha bit: Wonder maps it to B5G5R5A1 (unverified). A1BGR5 follows its name order (A, B, G, R from the top).
    {"BGR565", NVN_FORMAT_BGR565, 2, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, false,
     VK_FORMAT_B5G6R5_UNORM_PACK16, VK_FORMAT_UNDEFINED},
    {"BGR5", NVN_FORMAT_BGR5, 2, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, false,
     VK_FORMAT_B5G5R5A1_UNORM_PACK16, VK_FORMAT_UNDEFINED},
    {"BGR5A1", NVN_FORMAT_BGR5A1, 2, 1, 1, kAspectColor, 4, false, false, false, false, false, false, false, false,
     VK_FORMAT_B5G5R5A1_UNORM_PACK16, VK_FORMAT_UNDEFINED},
    // Wonder maps A1BGR5 to A1R5G5B5; its name order (A, B, G, R) gives A1B5G5R5. Unverified on hardware.
    {"A1BGR5", NVN_FORMAT_A1BGR5, 2, 1, 1, kAspectColor, 4, false, false, false, false, false, false, false, false,
     VK_FORMAT_A1B5G5R5_UNORM_PACK16, VK_FORMAT_UNDEFINED},
    {"BGRX8", NVN_FORMAT_BGRX8, 4, 1, 1, kAspectColor, 3, false, false, false, false, false, false, false, false,
     VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_UNDEFINED},
    {"BGRA8", NVN_FORMAT_BGRA8, 4, 1, 1, kAspectColor, 4, false, false, false, false, false, false, false, false,
     VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_UNDEFINED},
    {"BGRX8_SRGB", NVN_FORMAT_BGRX8_SRGB, 4, 1, 1, kAspectColor, 3, true, false, false, false, false, false, false, false,
     VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_UNDEFINED},
    {"BGRA8_SRGB", NVN_FORMAT_BGRA8_SRGB, 4, 1, 1, kAspectColor, 4, true, false, false, false, false, false, false, false,
     VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_UNDEFINED},
};

constexpr int kCount = static_cast<int>(sizeof(kTable) / sizeof(kTable[0]));

// Vertex attribute formats: the 1..4 component families, RGB10A2 (0x3D, 0x3E, 0x69..0x6C), R11G11B10F, the scaled
// UI2F/I2F forms and the RGBX family. Depth, stencil, compressed, sRGB and the other packed 16-bit values are not
// vertex data.
bool vertex_capable(int nvn_format) {
    return (nvn_format >= NVN_FORMAT_R8 && nvn_format <= NVN_FORMAT_RGBA32I) ||
           (nvn_format >= NVN_FORMAT_R8_UI2F && nvn_format <= NVN_FORMAT_RGBX32I) ||
           nvn_format == NVN_FORMAT_RGB10A2 || nvn_format == NVN_FORMAT_RGB10A2UI ||
           nvn_format == NVN_FORMAT_R11G11B10F;
}

}  // namespace

const FormatInfo* format_info(int nvn_format) {
    if (nvn_format < 1 || nvn_format > kCount) return nullptr;
    const FormatInfo* f = &kTable[nvn_format - 1];
    return f->nvn == nvn_format ? f : nullptr;
}

int format_count() { return kCount; }

const FormatInfo* format_at(int index) {
    if (index < 0 || index >= kCount) return nullptr;
    return &kTable[index];
}

VkFormat vertex_format(int nvn_format) {
    const FormatInfo* f = format_info(nvn_format);
    if (!f || !vertex_capable(nvn_format)) return VK_FORMAT_UNDEFINED;
    return f->vk;
}

}  // namespace nxrt::nvn

namespace nxrt::nvn {

bool vk_format_is_signed_int(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8_SINT: case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16_SINT: case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_A2B10G10R10_SINT_PACK32:
            return true;
        default:
            return false;
    }
}

bool vk_format_is_integer(VkFormat f) {
    if (vk_format_is_signed_int(f)) return true;
    switch (f) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32:
            return true;
        default:
            return false;
    }
}

}  // namespace nxrt::nvn
