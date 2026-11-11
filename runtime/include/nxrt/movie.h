// SPDX-License-Identifier: GPL-3.0-or-later
// Host implementation of the platform's multimedia library (the `movie::` API of the SDK, a port of Android's stagefright) on Windows
// Media Foundation. The game imports 41 functions of the library (Extractor, Decoder, MediaData, Buffer, a few free functions); this
// module registers a host handler for each of them and forces the imports to the host (module_override_symbol), so the library that
// ships in subsdk0 (and the NVDEC/VIC driver calls under it) never runs.
//
//   [media] movie = mf (default) | off     off keeps the lifted subsdk0 library
//   [media] movie_log = 1                  one log line per decoder call (very verbose)
//
// Approach and protocol notes: smo-recomp runtime/services/nnmovie_mf.c (GPL-3.0). The game drives the library in a fixed way:
// an Extractor demuxes a movie file the game reads through its own ClientStreamReader, two Decoders (H.264 video as NV12, AAC audio
// as 16-bit PCM) turn the compressed samples into frames. Here the source reader of Media Foundation does the demuxing and the
// decoding in a worker thread; the "compressed sample" the extractor hands to a decoder is a token that stands for a decoded sample.
// The decoded NV12 frames and PCM blocks are copied into the buffers the game owns.
#pragma once
#include <cstddef>
#include <cstdint>

namespace nxrt::media {

// Registers the host handlers for the movie:: imports and forces the imports to them. False when Media Foundation is unavailable.
// Safe to call more than once.
bool install_movie_hle();

// The mangled names of the 41 imports the handlers answer (for tests and docs).
const char* const* movie_import_names(size_t* count);

struct MovieStats {
    uint64_t extractors_created = 0;
    uint64_t extractors_destroyed = 0;
    uint64_t decoders_created = 0;
    uint64_t decoders_destroyed = 0;
    uint64_t video_frames_delivered = 0;   // copied into a game buffer
    uint64_t audio_blocks_delivered = 0;
    uint64_t audio_bytes_delivered = 0;
    uint64_t samples_read = 0;             // decoded samples taken from Media Foundation
    uint64_t read_errors = 0;
};
MovieStats movie_stats();

}  // namespace nxrt::media
