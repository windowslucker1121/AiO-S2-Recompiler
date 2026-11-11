// SPDX-License-Identifier: GPL-3.0-or-later
// Button prompts (S17): replaces the pixels of the game's button icon textures with Xbox, PlayStation or keyboard and mouse icons.
//
// How it works: the game draws its prompts from 19 small BC7 textures (prompt_resolve.h). The graphics back end asks a hook
// (nxrt/tex_override.h) about every small single-level BC7 sRGB texture it uploads; the hook recognises an icon by a hash of the
// texture's tiled bytes, which it learned at first use by reading the same textures from the player's own paks (the RomFS folder), and
// answers with the pixels of the pack entry the resolver picked. When the style, the context, the bindings or the last used device change,
// the hook's answers change and every small BC7 texture is uploaded again.
//
// The icon pack (assets/prompts/prompt_icons.pak, built by scripts/build_prompt_pack.py from Kenney's CC0 Input Prompts) is searched in
// [prompts] dir, <exe folder>/prompts/, assets/prompts/ under the working directory and the exe folder.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "prompt_resolve.h"

namespace nxrt::play {

// The icon pack file: 36x36 straight-alpha RGBA8 entries by name (see scripts/build_prompt_pack.py for the layout).
class IconPack {
public:
    bool load_file(const std::string& path);                         // false when missing or malformed (error() says why)
    bool load_memory(std::vector<uint8_t> bytes);
    const uint8_t* find(const std::string& name) const;              // w*h*4 bytes, or null
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    size_t size() const { return index_.size(); }
    const std::string& error() const { return err_; }
private:
    struct Entry { std::string name; uint32_t off; };
    std::vector<uint8_t> data_;
    std::vector<Entry> index_;                                       // sorted by name
    uint32_t w_ = 0, h_ = 0;
    std::string err_;
};

// FNV-1a over a byte range: the identity of an icon texture (its tiled bytes as the game wrote them).
uint64_t prompt_hash(const void* data, size_t size);

struct PromptInputs {
    PromptStyle requested = PromptStyle::Console;       // [prompts] style
    bool label_abxy = true;
    Ctx ctx = Ctx::Menu;
    uint64_t settings_epoch = 0;                       // changes whenever the bindings or the [switch] table may have changed
    LastDevice last = LastDevice::None;
    int last_pad_kind = 0;                             // input::PadKind of the pad used last
    int connected_pad_kind = -1;                       // kind of the first connected pad, -1 without one
};

// Main thread, once per frame (cheap when nothing changed): works out the style that shows now and the icon for every game icon, and
// asks the graphics back end to upload the icon textures again when an answer changed. `s` is read only when the keyboard style shows.
void prompts_update(const Settings& s, const PromptInputs& in);
PromptStyle prompts_effective_style();                  // the style that shows now (never Auto)
const char* prompts_effective_name();                   // its name for the settings page
std::string prompts_icon_name(Icon i);                  // the pack entry in use for a game icon ("" = the game's own)
void prompts_reset();                                   // tests: drop all state and the hook

}  // namespace nxrt::play
