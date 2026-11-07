/* astc2bc7: ASTC (LDR / sRGB) -> BC7 transcoder library. GPL-3.0-or-later (see ../README.md for borrowed code).
 *
 * Pipeline: decode ASTC to RGBA8 with ARM astcenc (Apache-2.0, loaded as a per-ISA DLL chosen by CPUID), then encode
 * BC7 with bc7e (Apache-2.0, Binomial LLC, compiled with ISPC). Both stages are multithreaded. Block-compressed (BCn)
 * input is passed through byte for byte. sRGB is handled by decoding in the sRGB profile (RGB stays sRGB-encoded,
 * alpha stays linear), encoding BC7 on those 8-bit values and tagging the output BC7_UNORM_SRGB.
 */
#ifndef ASTC2BC7_H
#define ASTC2BC7_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum a2b_status {
    A2B_OK = 0,
    A2B_ERR_ARGS,         /* null pointer, zero size, unsupported block size */
    A2B_ERR_SIZE,         /* input or output buffer size does not match the dimensions */
    A2B_ERR_UNSUPPORTED,  /* format that cannot be handled (HDR ASTC, 3D blocks, ...) */
    A2B_ERR_NO_ASTC_LIB,  /* no astcdec-*-shared.dll found next to the executable / in dll_dir */
    A2B_ERR_DECODE,       /* astcenc reported an error */
} a2b_status;

typedef enum a2b_format {
    A2B_ASTC_UNORM = 1, /* ASTC LDR, linear profile */
    A2B_ASTC_SRGB,      /* ASTC LDR, sRGB profile */
    A2B_BC1_UNORM, A2B_BC1_SRGB,
    A2B_BC2_UNORM, A2B_BC2_SRGB,
    A2B_BC3_UNORM, A2B_BC3_SRGB,
    A2B_BC4_UNORM, A2B_BC4_SNORM,
    A2B_BC5_UNORM, A2B_BC5_SNORM,
    A2B_BC6H_UF16, A2B_BC6H_SF16,
    A2B_BC7_UNORM, A2B_BC7_SRGB,
} a2b_format;

typedef struct a2b_input {
    a2b_format format;
    uint32_t width, height;   /* texels of the mip level */
    uint32_t block_w, block_h; /* ASTC footprint 4x4 .. 12x12; ignored for BCn */
} a2b_input;

typedef struct a2b_options {
    int quality;     /* bc7e preset 0 ultrafast, 1 veryfast, 2 fast, 3 basic, 4 slow, 5 veryslow, 6 slowest; -1 = default */
    int threads;     /* 0 = all hardware threads */
    int perceptual;  /* bc7e perceptual weights (default 0: minimise plain RGB error against the ASTC decode) */
    int allow_opaque_mode6; /* opaque source blocks: 0 (default) alpha exactly 255 (mode 6 off, the preset's channel rotations kept and the
                           swapped-in alpha slot pinned); 1 raw bc7e (alpha may drift to 254..244); 2 legacy (mode 6 and rotation
                           off, 2 to 12 dB lower RGB PSNR on RG normal maps) */
} a2b_options;

typedef struct a2b_stats {
    double decode_seconds, encode_seconds;
    const char* astc_isa;  /* "avx2", "sse4.1" or "sse2" */
} a2b_stats;

#define A2B_DEFAULT_QUALITY 5

/* Optional. Locates astcdec-{avx2,sse4.1,sse2}-shared.dll in dll_dir (NULL: the executable's directory) and picks the
 * best one the CPU supports. Called implicitly by the first transcode with the default directory. */
a2b_status a2b_init(const char* dll_dir);

const char* a2b_status_string(a2b_status s);
int a2b_is_astc(a2b_format f);
int a2b_is_srgb(a2b_format f);

/* Bytes of one block of a BCn format (8 or 16); 0 for ASTC/unknown. */
uint32_t a2b_bcn_block_bytes(a2b_format f);
/* Size in bytes of the output of a2b_transcode: BC7 for ASTC input, the input size for BCn. 0 on bad arguments. */
uint64_t a2b_output_size(const a2b_input* in);
/* Size in bytes of the ASTC data for the given dimensions. */
uint64_t a2b_astc_size(uint32_t w, uint32_t h, uint32_t bw, uint32_t bh);

/* ASTC -> BC7, BCn -> identical BCn. *out_format receives the format of out (BC7_UNORM / BC7_SRGB, or the input's). */
a2b_status a2b_transcode(const a2b_input* in, const void* data, size_t data_size, const a2b_options* opt,
                         a2b_format* out_format, void* out, size_t out_capacity, a2b_stats* stats);

/* The two halves, usable on their own. rgba is width*height*4 bytes, tightly packed. */
a2b_status a2b_decode_astc_rgba8(const a2b_input* in, const void* data, size_t data_size, int threads, uint8_t* rgba,
                                 a2b_stats* stats);
/* BC7 blocks whose alpha can decode only to values <= max_alpha get alpha exactly 0 (modes 4..7; colour kept up to the LSB of modes 6
 * and 7). For transcodes cached before the encoder pinned fully transparent blocks itself (bc7e left alpha 1..4 there). Returns the
 * number of blocks changed. */
size_t a2b_bc7_zero_faint_alpha(void* blocks, size_t count, int max_alpha);

a2b_status a2b_encode_bc7(uint32_t width, uint32_t height, const uint8_t* rgba, const a2b_options* opt, void* out,
                          size_t out_capacity, a2b_stats* stats);

#ifdef __cplusplus
}
#endif
#endif
