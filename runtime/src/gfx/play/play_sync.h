// SPDX-License-Identifier: GPL-3.0-or-later
// The play settings file shared with other programs (runtime/docs/MENU.md, "One settings file"): the library's Settings page and a text editor
// edit controls.ini while the game runs. The game
//   - notices a change made by someone else (a size / write-time poll, then the text compared with what it last read or wrote) and takes over
//     exactly the keys that changed (current settings + (file - last read)), so its own live state stays;
//   - writes only the keys it changed itself, in place in the file's current text (comments, unknown keys and other programs' changes stay),
//     and a change someone else made in the meantime is merged first, never overwritten;
//   - does not take its own write for an outside change.
// The key-level helpers are pure and tested (play_sync group); SettingsFile does the file work.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "play_types.h"

namespace nxrt::play {

// One change of the settings file at key level. Sections compare case-insensitively; [bindings.*] sections are lists (the same input may appear
// twice, the order matters), so they change as a whole.
struct IniChange {
    enum class Kind : uint8_t { Set, Remove, Section };
    Kind kind = Kind::Set;
    std::string section;                  // lower case, without brackets
    std::string key, value;               // Set / Remove (key in the file's spelling of `other`)
    std::vector<std::string> lines;       // Section: the section's key lines of `other`; empty: the section goes
};
// What turns `base` into `other` (both texts in the settings file format).
std::vector<IniChange> ini_diff(const std::string& base, const std::string& other);
// Applies the changes to `text` in place: values replaced on their own lines, new keys after the section's last key, new sections at the end.
std::string ini_apply(const std::string& text, const std::vector<IniChange>& changes);
// current + (other - base), all as Settings (through their canonical text). False when the result does not parse (*out = current).
bool merge_settings(const Settings& base, const Settings& other, const Settings& current, Settings* out);

class SettingsFile {
public:
    void set_path(const std::string& path);
    const std::string& path() const { return path_; }

    // The file as it is now is what the game has (after its own load or full write). Reads the text and the time stamp.
    void note_current();
    // The settings the file held when the game last read or wrote it.
    Settings base() const;

    // Cheap: the file's size and write time against the last seen ones; on a change, the text against the last read or written one.
    // Changed: someone else changed the file, *disk holds its settings (the base is not moved: merge first, then accept()).
    enum class Poll { Same, Changed, Unreadable };
    Poll poll(Settings* disk, std::string* err);
    void accept(const Settings& disk);    // the outside change is taken over: it is the new base

    // Writes the keys in which `mine` differs from the base into the file's current text. A change someone else made since the last read or
    // write is kept: *external is then true and *merged holds the file's settings after the write (mine + theirs). Nothing to write: no write.
    bool save(const Settings& mine, std::string* err, bool* external = nullptr, Settings* merged = nullptr);

private:
    struct Stamp {
        uint64_t size = 0, time = 0;
        bool exists = false;
        bool operator==(const Stamp& o) const { return size == o.size && time == o.time && exists == o.exists; }
    };
    static Stamp stamp_of(const std::string& path);
    static bool read_text(const std::string& path, std::string* out);
    mutable std::mutex mu_;
    std::string path_;
    std::string text_;                   // the text last read or written
    Settings base_;
    bool have_base_ = false;
    Stamp stamp_;
};

}  // namespace nxrt::play
