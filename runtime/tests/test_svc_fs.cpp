// SPDX-License-Identifier: GPL-3.0-or-later
// fsp-srv at IPC level: RomFS storage, read-only data file system, save data (create / open / write / commit / drop), SD card, account
// save API. Request bytes follow Switchbrew "Filesystem services" / libnx fs_service.c layouts.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/services.h"
#include "nxrt/services_api.h"
#include "svc_test_util.h"

NXTEST_GROUP(svc_fs)

namespace stdfs = std::filesystem;
using nxsvc::Args;
using nxsvc::Reply;
using nxsvc::Session;

namespace {

constexpr uint32_t kPathNotFound = 0x202, kPathExists = 0x402, kTargetNotFound = 0x7D402, kSdNoDevice = 0xFA202, kOutOfRange = 0x177A02;
constexpr uint32_t kNoAppend = 0x307202;

struct Fixture {
    stdfs::path dir, rom, save;
    nxsvc::Env env;
    Fixture() {
        dir = stdfs::temp_directory_path() / ("nxrt_svcfs_" + std::to_string(GetCurrentProcessId()));
        std::error_code ec;
        stdfs::remove_all(dir, ec);
        rom = dir / "romfs";
        save = dir / "save";
        stdfs::create_directories(rom / "Worlds", ec);
        stdfs::create_directories(save, ec);
        write(rom / "a.bin", std::string(1000, 'a'));
        write(rom / "Worlds" / "b.pak", "bee");
        nxrt::config_set("fs", "romfs", rom.string().c_str());
        nxrt::config_set("fs", "save_dir", save.string().c_str());
        nxrt::services::reset_all();
    }
    ~Fixture() {
        nxrt::config_clear();
        nxrt::services::reset_all();
        std::error_code ec;
        stdfs::remove_all(dir, ec);
    }
    static void write(const stdfs::path& p, const std::string& s) {
        std::ofstream o(p, std::ios::binary);
        o << s;
    }
};

// A 0x301-byte path buffer for the A/X buffer of the IFileSystem commands.
struct PathBuf {
    char b[0x301] = {};
    explicit PathBuf(const char* s) { std::strncpy(b, s, 0x300); }
};

// SaveDataAttribute (0x40): application id, uid (hi, lo), system save data id, type, rank, index.
std::vector<uint8_t> attr(uint8_t type, uint64_t uid_hi, uint64_t uid_lo, uint16_t index = 0) {
    std::vector<uint8_t> a(0x40, 0);
    const uint64_t app = 0;   // 0 = the calling title
    std::memcpy(&a[0], &app, 8);
    std::memcpy(&a[8], &uid_hi, 8);
    std::memcpy(&a[16], &uid_lo, 8);
    a[32] = type;
    std::memcpy(&a[34], &index, 2);
    return a;
}

Session open_fsp() {
    Session s = nxsvc::connect("fsp-srv");
    CHECK(s.valid());
    CHECK(s.call(1, Args().with_pid().u64(0)).ok());   // SetCurrentProcess(pid)
    return s;
}

// CreateSaveDataFileSystem(attr, creation info, meta info): creation info = size, journal size, available, owner, flags, space id.
Reply create_save(Session& s, const std::vector<uint8_t>& a, int64_t size, int64_t journal) {
    Args args;
    args.bytes(a.data(), a.size());
    args.i64(size).i64(journal).u64(size).u64(0x010012101468C000ull).u32(0).u8(1).pad(0x1B);   // creation info, 0x40 bytes
    args.pad(0x10);                                                                          // meta info
    return s.call(22, args);
}

Reply open_save(Session& s, const std::vector<uint8_t>& a, uint8_t space = 1, uint32_t cmd = 51) {
    Args args;
    args.u8(space).pad(7).bytes(a.data(), a.size());
    return s.call(cmd, args);
}

Reply fs_call_path(Session& fsys, uint32_t cmd, const char* path, Args extra = {}) {
    auto pb = std::make_unique<PathBuf>(path);   // heap: the X descriptor holds 39-bit addresses
    extra.buf_x(pb->b, 0x301);
    return fsys.call(cmd, extra);
}

}  // namespace

NXTEST(fsp_set_current_process_and_log_mode) {
    Fixture f;
    Session s = open_fsp();
    Reply r = s.call(1005);
    CHECK(r.ok());
    CHECK_EQ(r.get<uint32_t>(0), 0u);
    r = s.call(1011);                        // GetProgramIndexForAccessLog
    CHECK(r.ok());
    r = s.call(27);                          // IsExFatSupported
    CHECK(r.ok());
    CHECK_EQ(r.get<uint8_t>(0), 0u);
}

NXTEST(fsp_unknown_command_is_stub) {
    Fixture f;
    Session s = open_fsp();
    CHECK_EQ(s.call(9999).result, nxrt::kernel::kStubService);
}

NXTEST(rom_storage_header_and_file_data) {
    Fixture f;
    Session s = open_fsp();
    Reply r = s.call(200);
    REQUIRE(r.ok());
    Session st = s.sub(r);
    REQUIRE(st.valid());
    r = st.call(4);                          // IStorage::GetSize
    REQUIRE(r.ok());
    const int64_t total = r.get<int64_t>(0);
    CHECK(total > 0x200 + 1003);
    uint8_t hdr[0x50] = {};
    r = st.call(0, Args().i64(0).i64(0x50).buf_b(hdr, 0x50));
    CHECK(r.ok());
    uint64_t f64[10];
    std::memcpy(f64, hdr, 0x50);
    CHECK_EQ(f64[0], 0x50u);                 // header size
    const uint64_t dir_hash = f64[1], dir_tab = f64[3], file_hash = f64[5], file_tab = f64[7], data = f64[9];
    CHECK_EQ(data, 0x200u);
    CHECK(dir_hash > data && dir_tab > dir_hash && file_hash > dir_tab && file_tab > file_hash);
    CHECK(file_tab + f64[8] <= static_cast<uint64_t>(total));
    // root directory entry: first entry of the directory table; its first file is a.bin (sorted), with the data we wrote
    uint32_t root[6] = {};
    r = st.call(0, Args().i64(static_cast<int64_t>(dir_tab)).i64(24).buf_b(root, 24));
    CHECK(r.ok());
    CHECK_EQ(root[1], 0xFFFFFFFFu);          // no sibling
    CHECK(root[2] != 0xFFFFFFFFu);           // child directory (Worlds)
    CHECK(root[3] != 0xFFFFFFFFu);           // child file (a.bin)
    uint8_t fe[0x20 + 8] = {};
    r = st.call(0, Args().i64(static_cast<int64_t>(file_tab + root[3])).i64(0x28).buf_b(fe, 0x28));
    CHECK(r.ok());
    // file entry (Switchbrew RomFS): parent u32, sibling u32, data offset u64, data size u64, hash u32, name length u32, name
    uint64_t off, size;
    uint32_t name_len;
    std::memcpy(&off, fe + 0x08, 8);
    std::memcpy(&size, fe + 0x10, 8);
    std::memcpy(&name_len, fe + 0x1C, 4);
    CHECK_EQ(size, 1000u);
    CHECK_EQ(name_len, 5u);
    char content[1000];
    r = st.call(0, Args().i64(static_cast<int64_t>(data + off)).i64(1000).buf_b(content, 1000));
    CHECK(r.ok());
    CHECK_EQ(std::string(content, 1000), std::string(1000, 'a'));
}

NXTEST(rom_storage_read_out_of_range) {
    Fixture f;
    Session s = open_fsp();
    Session st = s.sub(s.call(200));
    REQUIRE(st.valid());
    const int64_t total = st.call(4).get<int64_t>(0);
    char buf[16];
    CHECK_EQ(st.call(0, Args().i64(total - 8).i64(16).buf_b(buf, 16)).result, kOutOfRange);
    CHECK(st.call(0, Args().i64(total - 16).i64(16).buf_b(buf, 16)).ok());
    CHECK_EQ(st.call(1, Args().i64(0).i64(1).buf_a(buf, 1)).result, 0x313802u);   // writes to the RomFS are refused
}

NXTEST(rom_missing_folder_is_target_not_found) {
    Fixture f;
    nxrt::config_set("fs", "romfs", (f.dir / "does_not_exist").string().c_str());
    nxrt::services::reset_all();
    Session s = open_fsp();
    CHECK_EQ(s.call(200).result, kTargetNotFound);
}

NXTEST(rom_file_system_read_only) {
    Fixture f;
    Session s = open_fsp();
    Reply r = s.call(2);                     // OpenDataFileSystemByCurrentProcess
    REQUIRE(r.ok());
    Session fsys = s.sub(r);
    // GetEntryType
    r = fs_call_path(fsys, 7, "/a.bin");
    CHECK(r.ok());
    CHECK_EQ(r.get<uint32_t>(0), 1u);
    r = fs_call_path(fsys, 7, "/Worlds");
    CHECK_EQ(r.get<uint32_t>(0), 0u);
    CHECK_EQ(fs_call_path(fsys, 7, "/nope").result, kPathNotFound);
    // OpenFile + Read
    r = fs_call_path(fsys, 8, "/Worlds/b.pak", Args().u32(1));
    REQUIRE(r.ok());
    Session file = fsys.sub(r);
    char buf[16] = {};
    r = file.call(0, Args().u32(0).pad(4).i64(0).i64(16).buf_b(buf, 16));
    CHECK(r.ok());
    CHECK_EQ(r.get<uint64_t>(0), 3u);
    CHECK_EQ(std::string(buf, 3), std::string("bee"));
    CHECK_EQ(file.call(4).get<int64_t>(0), 3);
    // refuses writes
    CHECK_EQ(fs_call_path(fsys, 1, "/a.bin").result, 0x313802u);
    // OpenDirectory (files and directories), read entries
    r = fs_call_path(fsys, 9, "/", Args().u32(3));
    REQUIRE(r.ok());
    Session dir = fsys.sub(r);
    CHECK_EQ(dir.call(1).get<int64_t>(0), 2);
    std::vector<uint8_t> ents(0x310 * 4, 0xEE);
    r = dir.call(0, Args().buf_b(ents.data(), ents.size()));
    CHECK(r.ok());
    CHECK_EQ(r.get<int64_t>(0), 2);
    CHECK_EQ(std::string(reinterpret_cast<char*>(ents.data())), std::string("Worlds"));
    CHECK_EQ(ents[0x304], 0u);
    CHECK_EQ(std::string(reinterpret_cast<char*>(ents.data() + 0x310)), std::string("a.bin"));
    CHECK_EQ(ents[0x310 + 0x304], 1u);
    int64_t fsz;
    std::memcpy(&fsz, &ents[0x310 + 0x308], 8);
    CHECK_EQ(fsz, 1000);
}

NXTEST(sd_card_is_absent) {
    Fixture f;
    Session s = open_fsp();
    CHECK_EQ(s.call(18).result, kSdNoDevice);
    Session op = s.sub(s.call(400));
    REQUIRE(op.valid());
    Reply r = op.call(0);
    CHECK(r.ok());
    CHECK_EQ(r.get<uint8_t>(0), 0u);
    r = s.call(631);
    CHECK_EQ(r.get<uint8_t>(0), 0u);
}

NXTEST(save_open_missing_then_create_open_write_commit) {
    Fixture f;
    Session s = open_fsp();
    const std::vector<uint8_t> a = attr(1, 0x1111, 0x2222);
    CHECK_EQ(open_save(s, a).result, kTargetNotFound);
    CHECK(create_save(s, a, 4 << 20, 1 << 20).ok());
    CHECK_EQ(create_save(s, a, 4 << 20, 1 << 20).result, kPathExists);
    Reply r = open_save(s, a);
    REQUIRE(r.ok());
    {
        Session fsys = s.sub(r);
        CHECK(fs_call_path(fsys, 0, "/slot0", Args().u32(0).pad(4).i64(16)).ok());      // CreateFile(option, size)
        r = fs_call_path(fsys, 8, "/slot0", Args().u32(6));                                // write | append
        REQUIRE(r.ok());
        Session file = fsys.sub(r);
        const char data[16] = {'M', 'P', 'R', 'S', 'A', 'V', 'E', '0', 1, 2, 3, 4, 5, 6, 7, 8};
        CHECK(file.call(1, Args().u32(1).pad(4).i64(0).i64(16).buf_a(data, 16)).ok());    // Write with the flush option
        CHECK_EQ(file.call(1, Args().u32(0).pad(4).i64(16).i64(8).buf_a(data, 8)).ok(), true);
        CHECK(fsys.call(10).ok());                                                          // Commit
        CHECK(fs_call_path(fsys, 0, "/slot1", Args().u32(0).pad(4).i64(4)).ok());          // not committed
    }
    // both sessions closed: slot1 is gone, slot0 stays
    r = open_save(s, a);
    REQUIRE(r.ok());
    Session fsys = s.sub(r);
    Reply t = fs_call_path(fsys, 7, "/slot0");
    CHECK(t.ok());
    CHECK_EQ(t.get<uint32_t>(0), 1u);
    CHECK_EQ(fs_call_path(fsys, 7, "/slot1").result, kPathNotFound);
    r = fs_call_path(fsys, 8, "/slot0", Args().u32(1));
    REQUIRE(r.ok());
    Session file = fsys.sub(r);
    CHECK_EQ(file.call(4).get<int64_t>(0), 24);
    char buf[24] = {};
    CHECK(file.call(0, Args().u32(0).pad(4).i64(0).i64(24).buf_b(buf, 24)).ok());
    CHECK_EQ(std::string(buf, 8), std::string("MPRSAVE0"));
    // write without the append mode past the end
    r = fs_call_path(fsys, 8, "/slot0", Args().u32(2));
    Session w = fsys.sub(r);
    CHECK_EQ(w.call(1, Args().u32(0).pad(4).i64(20).i64(8).buf_a(buf, 8)).result, kNoAppend);
}

NXTEST(save_disk_layout_is_a_folder_under_save_dir) {
    Fixture f;
    Session s = open_fsp();
    CHECK(create_save(s, attr(1, 5, 6), 4 << 20, 1 << 20).ok());
    CHECK(stdfs::is_directory(f.save / "account" / "05000000000000000600000000000000"));
    CHECK(stdfs::exists(f.save / "account" / "05000000000000000600000000000000.meta"));
    CHECK(create_save(s, attr(3, 0, 0), 1 << 20, 1 << 18).ok());       // device save
    CHECK(stdfs::is_directory(f.save / "device"));
    CHECK(create_save(s, attr(5, 0, 0, 2), 1 << 20, 1 << 18).ok());    // cache storage index 2
    CHECK(stdfs::is_directory(f.save / "cache" / "2"));
}

NXTEST(save_info_reader_lists_saves) {
    Fixture f;
    Session s = open_fsp();
    CHECK(create_save(s, attr(1, 0xAA, 0xBB), 4 << 20, 1 << 20).ok());
    Reply r = s.call(61, Args().u8(1));      // OpenSaveDataInfoReaderBySaveDataSpaceId
    REQUIRE(r.ok());
    Session rd = s.sub(r);
    std::vector<uint8_t> infos(0x60 * 4, 0);
    r = rd.call(0, Args().buf_b(infos.data(), infos.size()));
    CHECK(r.ok());
    CHECK_EQ(r.get<int64_t>(0), 1);
    CHECK_EQ(infos[9], 1u);                   // type: account
    uint64_t hi, size;
    std::memcpy(&hi, &infos[0x10], 8);
    std::memcpy(&size, &infos[0x30], 8);
    CHECK_EQ(hi, 0xAAu);
    CHECK_EQ(size, static_cast<uint64_t>(4 << 20));
    r = rd.call(0, Args().buf_b(infos.data(), infos.size()));
    CHECK_EQ(r.get<int64_t>(0), 0);           // exhausted
}

NXTEST(save_delete_by_attribute) {
    Fixture f;
    Session s = open_fsp();
    const auto a = attr(1, 7, 8);
    CHECK(create_save(s, a, 1 << 20, 1 << 18).ok());
    Args args;
    args.u8(1).pad(7).bytes(a.data(), a.size());
    CHECK(s.call(28, args).ok());
    CHECK_EQ(open_save(s, a).result, kTargetNotFound);
    CHECK_EQ(s.call(28, args).result, kTargetNotFound);
}

NXTEST(save_on_sd_space_has_no_device) {
    Fixture f;
    Session s = open_fsp();
    CHECK_EQ(open_save(s, attr(1, 1, 1), 4).result, kSdNoDevice);
}

NXTEST(account_save_api) {
    Fixture f;
    bool created = false;
    int64_t size = 0, journal = 0;
    CHECK_EQ(nxrt::services::fsapi::account_save_size(1, 2, &size, &journal), kTargetNotFound);
    CHECK_EQ(nxrt::services::fsapi::ensure_account_save(1, 2, &created), 0u);
    CHECK(created);
    CHECK_EQ(nxrt::services::fsapi::ensure_account_save(1, 2, &created), 0u);
    CHECK(!created);
    CHECK_EQ(nxrt::services::fsapi::account_save_size(1, 2, &size, &journal), 0u);
    // the title's sizes (control.nacp UserAccountSaveDataSize / JournalSize)
    CHECK(nxrt::services::profile().account_save_size > 0);
    CHECK_EQ(size, static_cast<int64_t>(nxrt::services::profile().account_save_size));
    CHECK_EQ(journal, static_cast<int64_t>(nxrt::services::profile().account_save_journal));
    // the same save is then openable through fsp-srv with the attribute the SDK builds for MountSaveData
    Session s = open_fsp();
    CHECK(open_save(s, attr(1, 1, 2)).ok());
}

NXTEST(multi_commit_manager_opens) {
    Fixture f;
    Session s = open_fsp();
    const auto a = attr(1, 3, 4);
    CHECK(create_save(s, a, 1 << 20, 1 << 18).ok());
    Session fsys = s.sub(open_save(s, a));
    REQUIRE(fsys.valid());
    CHECK(fs_call_path(fsys, 2, "/d").ok());
    // IMultiCommitManager::Add takes the file system as an object argument: the interface travels as a move handle
    Session mgr = s.sub(s.call(1200));
    REQUIRE(mgr.valid());
    // (Add / Commit are exercised through the file system's own Commit: the manager only forwards)
    CHECK(fsys.call(10).ok());
    CHECK(stdfs::is_directory(f.save / "account" / "03000000000000000400000000000000" / "d"));
}

// Saves made under an earlier save folder name (game::Info old_save_folders) are moved to the current one the first time it is asked for.
namespace {
stdfs::path fresh_base(const char* tag) {
    stdfs::path b = stdfs::temp_directory_path() / ("nxrt_savemig_" + std::string(tag) + "_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    stdfs::remove_all(b, ec);
    stdfs::create_directories(b, ec);
    return b;
}
std::string slurp(const stdfs::path& p) {
    std::ifstream i(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(i), std::istreambuf_iterator<char>());
}
}  // namespace

NXTEST(save_folder_migration_moves_an_old_folder) {
    const stdfs::path b = fresh_base("move");
    stdfs::create_directories(b / "OldName" / "save" / "account");
    Fixture::write(b / "OldName" / "save" / "account" / "slot.bin", "progress");
    const std::string got = nxrt::services::migrate_save_folder(b.string(), "new-name", "Missing; OldName");
    CHECK(stdfs::path(got) == b / "new-name");
    CHECK_EQ(slurp(b / "new-name" / "save" / "account" / "slot.bin"), std::string("progress"));
    CHECK(!stdfs::exists(b / "OldName"));
    // A second start finds the new folder and leaves it alone.
    CHECK(stdfs::path(nxrt::services::migrate_save_folder(b.string(), "new-name", "OldName")) == b / "new-name");
    std::error_code ec;
    stdfs::remove_all(b, ec);
}

NXTEST(save_folder_migration_keeps_an_existing_new_folder) {
    const stdfs::path b = fresh_base("keep");
    stdfs::create_directories(b / "OldName" / "save");
    stdfs::create_directories(b / "new-name" / "save");
    Fixture::write(b / "OldName" / "save" / "a.bin", "old");
    Fixture::write(b / "new-name" / "save" / "a.bin", "new");
    CHECK(stdfs::path(nxrt::services::migrate_save_folder(b.string(), "new-name", "OldName")) == b / "new-name");
    CHECK_EQ(slurp(b / "new-name" / "save" / "a.bin"), std::string("new"));
    CHECK_EQ(slurp(b / "OldName" / "save" / "a.bin"), std::string("old"));
    // Nothing to move: the new path, nothing created.
    CHECK(stdfs::path(nxrt::services::migrate_save_folder(b.string(), "other", "Absent")) == b / "other");
    CHECK(!stdfs::exists(b / "other"));
    std::error_code ec;
    stdfs::remove_all(b, ec);
}

// The default save folder of the registered game under a fake %APPDATA%: a folder with the game's old name is moved to the new one.
NXTEST(save_dir_moves_the_registered_games_old_save_folder) {
    const nxrt::game::Info& g = nxrt::game::info();
    std::string old = g.old_save_folders ? g.old_save_folders : "";
    if (old.find(';') != std::string::npos) old.resize(old.find(';'));
    if (old.empty() || !g.save_folder || !*g.save_folder) {
        nxtest::skip("the registered game names no old save folder");
        return;
    }
    const stdfs::path b = fresh_base("appdata");
    stdfs::create_directories(b / old / "save" / "device");
    Fixture::write(b / old / "save" / "device" / "d.bin", "device save");
    const char* prev = std::getenv("APPDATA");
    const std::string keep = prev ? prev : "";
    _putenv_s("APPDATA", b.string().c_str());
    nxrt::config_clear();
    const std::string dir = nxrt::services::save_dir();
    _putenv_s("APPDATA", keep.c_str());
    CHECK(stdfs::path(dir) == b / g.save_folder / "save");
    CHECK_EQ(slurp(b / g.save_folder / "save" / "device" / "d.bin"), std::string("device save"));
    CHECK(!stdfs::exists(b / old));
    std::error_code ec;
    stdfs::remove_all(b, ec);
}