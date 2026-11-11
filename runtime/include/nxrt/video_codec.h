// SPDX-License-Identifier: GPL-3.0-or-later
// Host implementation of the platform 2 SDK's hardware video decoders (nn::codec Vp9Decoder / Vp8Decoder, the decoder image pools) and of
// the sync objects their output is fenced with (nn::syncpt::sdk SyncPointEvent, nn::gs::TryWaitNativeSync), decoding with libvpx.
//
// Why this layer: on platform 2 every movie player (title A's movie:: compatibility library in subsdk0, the nn::movie player title C
// links into main) demuxes with the SDK's nn::movie::MediaExtractor and decodes with nn::codec, which drives NVDEC through the
// GraphicsSupervisor of the GPU boot nxrt skips. nn::codec is the narrowest interface both games import by name, so the demuxer, the
// players and their A/V clocks stay guest code and only the decoder and its fences answer on the host.
//
// Model (worked out from subsdk0's movie::VideoDecoder, see docs in video_codec.cpp): the player registers N output buffers (one image
// pool each), acquires each one with a fence, submits compressed frames, waits for the fences in acquisition order (a SyncPointEvent
// linked into an os::MultiWait, then TryWaitNativeSync), reads a finished image with ReadDecoderOutputImageNv12/Rgba, and releases
// and re-acquires the buffer. Here a submitted frame is decoded at once on the submitting thread; decoded pictures fill the acquired
// buffers first in first out, and a buffer's fence is signalled when its picture is in it.
//
//   [media] video_decoder = host | off      default host.
//                                          off keeps nn::movie::Initialize answering "unavailable" (src/core/switch2.cpp)
//   [media] video_decoder_threads = 4       libvpx worker threads per decoder
//   [media] video_decoder_log = 1           one log line per decoder call (verbose)
//   [media] movie_trace = 1                 default on: logs (rate-limited) what the SDK's nn::movie::MediaExtractor answers while a movie
//                                          opens (data appended, track count, container mime); answers are passed through unchanged
// A stats line (vcodec: stats ...) comes at most every 5 s while decoding: packets, pictures decoded, placed in output buffers, read by
// the game, errors and the first error.
#pragma once
#include <cstdint>

namespace nxrt::media {

// Registers the host handlers (platform 2 games only) and forces the imports to them. False when off, not platform 2, or built without
// libvpx (NXRT_HAVE_LIBVPX). Safe to call more than once.
bool install_video_codec_hle();

// True once install_video_codec_hle() routed the decoders to the host.
bool video_codec_enabled();

struct VideoCodecStats {
    uint64_t decoders_created = 0;
    uint64_t decoders_destroyed = 0;
    uint64_t packets_submitted = 0;
    uint64_t frames_decoded = 0;     // pictures libvpx returned
    uint64_t frames_delivered = 0;   // pictures placed in an acquired output buffer
    uint64_t frames_read = 0;        // ReadDecoderOutputImage* calls
    uint64_t decode_errors = 0;
};
VideoCodecStats video_codec_stats();

}  // namespace nxrt::media
