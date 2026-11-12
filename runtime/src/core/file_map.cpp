// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/file_map.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>

namespace nxrt {
namespace {

std::wstring widen_utf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

}  // namespace

MappedFile::~MappedFile() { close(); }

void MappedFile::close() {
    if (view_) UnmapViewOfFile(view_);
    if (map_) CloseHandle(map_);
    if (file_ && file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    view_ = map_ = file_ = nullptr;
    size_ = 0;
    file_size_ = 0;
}

bool MappedFile::open(const std::string& path, bool* exists) {
    close();
    if (exists) *exists = false;
    const std::wstring w = widen_utf8(path);
    HANDLE f = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;   // no file: nothing to read, not an error
    }
    if (exists) *exists = true;
    file_ = f;
    LARGE_INTEGER len{};
    if (!GetFileSizeEx(f, &len)) { close(); return false; }
    file_size_ = static_cast<uint64_t>(len.QuadPart);
    if (len.QuadPart == 0) return true;
    if (static_cast<uint64_t>(len.QuadPart) > static_cast<uint64_t>(SIZE_MAX)) { close(); return false; }
    map_ = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!map_) { const uint64_t fs = file_size_; close(); file_size_ = fs; return false; }
    view_ = MapViewOfFile(map_, FILE_MAP_READ, 0, 0, 0);
    if (!view_) { const uint64_t fs = file_size_; close(); file_size_ = fs; return false; }
    size_ = static_cast<size_t>(len.QuadPart);
    return true;
}

uint64_t write_file_mapped(const std::string& path, size_t bytes, const std::function<bool(void* view, size_t* bytes)>& fill) {
    if (!bytes) return 0;
    const std::wstring target = widen_utf8(path);
    const std::wstring tmp = target + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    const uint64_t want = bytes;
    HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READWRITE, static_cast<DWORD>(want >> 32), static_cast<DWORD>(want & 0xffffffffu), nullptr);
    bool ok = m != nullptr;
    size_t got = bytes;
    if (ok) {
        void* v = MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, bytes);
        ok = v != nullptr;
        if (ok) {
            ok = fill(v, &got) && got > 0 && got <= bytes;
            // the cache manager writes the dirty pages out; FlushViewOfFile only starts that early so the rename is not followed by
            // gigabytes of writes from the system's lazy writer at a random later time
            if (ok) FlushViewOfFile(v, 0);
            UnmapViewOfFile(v);
        }
        CloseHandle(m);
    }
    if (ok && got < bytes) {   // the mapping sized the file to `bytes`: cut it to what was written
        LARGE_INTEGER at{};
        at.QuadPart = static_cast<LONGLONG>(got);
        ok = SetFilePointerEx(f, at, nullptr, FILE_BEGIN) && SetEndOfFile(f);
    }
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return 0;
    }
    if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return 0;
    }
    return got;
}

}  // namespace nxrt
