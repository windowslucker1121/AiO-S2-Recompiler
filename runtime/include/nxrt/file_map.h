// SPDX-License-Identifier: GPL-3.0-or-later
// Big files through file mappings instead of heap buffers. Mapped file pages are backed by the file itself, so they are not charged to
// the process's private commit (what the memory guard watches, nxrt/memguard.h) and the system can drop or write them out under
// pressure. Used for the multi-gigabyte pipeline cache: a std::vector of its size added 4.5 GB of commit in one step at every save.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace nxrt {

// A read-only view of a whole file. Empty (data() null, size() 0) for an empty file or when the file could not be opened/mapped.
class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    // path in UTF-8. Returns false when the file exists but cannot be mapped (missing file: true, empty view, *exists false).
    bool open(const std::string& path, bool* exists = nullptr);
    void close();
    const char* data() const { return static_cast<const char*>(view_); }
    size_t size() const { return size_; }
    uint64_t file_size() const { return file_size_; }

private:
    void* file_ = nullptr;
    void* map_ = nullptr;
    void* view_ = nullptr;
    size_t size_ = 0;
    uint64_t file_size_ = 0;
};

// Writes `path` (UTF-8) with `bytes` bytes through a writable mapping of the new file: fill(view, &bytes) writes them and may lower
// `bytes` (the file is cut to it). The file is first written as `path`.tmp and renamed over `path` only when fill returned true and
// everything succeeded, so a process that ends in the middle leaves the old file intact. Returns the bytes written (0: nothing written).
uint64_t write_file_mapped(const std::string& path, size_t bytes, const std::function<bool(void* view, size_t* bytes)>& fill);

}  // namespace nxrt
