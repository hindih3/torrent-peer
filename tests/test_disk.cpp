#include "runtime/managers/disk_manager.hpp"

#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

// DiskManager expects a TorrentFile that parse_torrent has already accepted,
// so every torrent built here is one parse_torrent would accept too. Rejecting
// bad metadata (lengths, unsafe or duplicate paths) is tested in test_torrent.cpp.

namespace fs = std::filesystem;

static int failures = 0;

static void check(const bool good, const std::string& label) {
    std::printf("%s  %s\n", good ? "ok  " : "FAIL", label.c_str());
    if (!good) ++failures;
}

static void expect_throw(const std::string& label, const std::function<void()>& fn) {
    try {
        fn();
        std::printf("FAIL  expected throw: %s\n", label.c_str());
        ++failures;
    } catch (const std::exception& e) {
        std::printf("ok    %s -> %s\n", label.c_str(), e.what());
    }
}

// A fresh directory under /tmp, removed with everything in it at scope exit.
struct TempDir {
    fs::path path;

    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "test_disk.XXXXXX").string();
        if (!::mkdtemp(tmpl.data())) throw std::runtime_error("mkdtemp failed");
        path = tmpl;
    }
    ~TempDir() { fs::remove_all(path); }

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// Only the number of piece hashes matters to DiskManager, not their contents.
static void set_pieces(TorrentFile& t) {
    const auto pl = static_cast<uint64_t>(t.piece_length);
    const uint64_t count = pl == 0 ? 0 : (t.total_length + pl - 1) / pl;
    t.pieces.assign(count, std::string(20, '\0'));
}

static TorrentFile make_single(const std::string& name, const int64_t piece_length,
                               const int64_t length) {
    TorrentFile t;
    t.name         = name;
    t.piece_length = piece_length;
    t.length       = length;
    t.total_length = static_cast<uint64_t>(length);
    set_pieces(t);
    return t;
}

static TorrentFile make_multi(const std::string& name, const int64_t piece_length,
                              const std::vector<FileInfo>& files) {
    TorrentFile t;
    t.name         = name;
    t.piece_length = piece_length;
    t.files        = files;
    for (const FileInfo& f : files) t.total_length += static_cast<uint64_t>(f.length);
    set_pieces(t);
    return t;
}

// Files named "f0", "f1", ... with the given lengths.
static TorrentFile make_multi(const std::string& name, const int64_t piece_length,
                              const std::vector<int64_t>& lengths) {
    std::vector<FileInfo> files;
    for (size_t i = 0; i < lengths.size(); ++i)
        files.push_back({lengths[i], {"f" + std::to_string(i)}});
    return make_multi(name, piece_length, files);
}

// Every byte of the torrent gets a value that depends on its position, so a
// byte written to the wrong place shows up as a mismatch.
static std::vector<uint8_t> expected(const uint64_t global_offset, const uint64_t len) {
    std::vector<uint8_t> out(len);
    for (uint64_t i = 0; i < len; ++i)
        out[i] = static_cast<uint8_t>((global_offset + i) * 7 + 3);
    return out;
}

static void write_all(DiskManager& dm, const TorrentFile& t) {
    const auto pl = static_cast<uint64_t>(t.piece_length);
    for (uint32_t i = 0; i < t.pieces.size(); ++i)
        dm.write_piece({i, expected(i * pl, piece_size(t, i))});
}

static std::vector<uint8_t> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Reads the files directly instead of going through read_block, so a mapping
// bug shared by reads and writes can't cancel itself out.
static bool on_disk_matches(const TorrentFile& t, const fs::path& dir) {
    if (!t.files) return read_file(dir / t.name) == expected(0, t.total_length);

    uint64_t cursor = 0;
    for (const FileInfo& f : *t.files) {
        fs::path p = dir / t.name;
        for (const std::string& c : f.path) p /= c;
        const auto len = static_cast<uint64_t>(f.length);
        if (read_file(p) != expected(cursor, len)) return false;
        cursor += len;
    }
    return true;
}

static bool reads_match(const DiskManager& dm, const TorrentFile& t) {
    const auto pl = static_cast<uint64_t>(t.piece_length);
    for (uint32_t i = 0; i < t.pieces.size(); ++i) {
        const auto len = static_cast<uint32_t>(piece_size(t, i));
        if (dm.read_block(i, 0, len) != expected(i * pl, len)) return false;
    }
    return true;
}

static void test_layouts() {
    struct Case {
        const char*  label;
        TorrentFile  t;
    };
    const std::vector<Case> cases = {
        {"single file, short last piece",        make_single("one.bin", 4, 10)},
        {"file ends on a piece boundary",        make_multi("aligned", 4, {4, 8})},
        {"piece spans two files",                make_multi("two", 4, {3, 5})},
        {"piece spans three files",              make_multi("three", 8, {1, 1, 6, 2})},
        {"zero-length files at start/mid/end",   make_multi("zeros", 4, {0, 3, 0, 5, 0})},
        {"nested directories",                   make_multi("nested", 4, std::vector<FileInfo>{
                                                     {3, {"a", "b", "x"}}, {6, {"a", "y"}}})},
    };

    for (const auto& [label, t] : cases) {
        TempDir dir;
        DiskManager dm(t, dir.path);
        write_all(dm, t);
        dm.sync();
        check(reads_match(dm, t),          std::string(label) + ": read_block round-trip");
        check(on_disk_matches(t, dir.path), std::string(label) + ": bytes on disk");
    }
}

// Every valid (piece, offset, length) on a layout with a zero-length file,
// a 1-byte file, pieces spanning files, and a short last piece.
static void test_exhaustive_reads() {
    const TorrentFile t = make_multi("exhaustive", 4, {3, 0, 5, 1, 6});
    TempDir dir;
    DiskManager dm(t, dir.path);
    write_all(dm, t);

    const auto pl = static_cast<uint64_t>(t.piece_length);
    int reads = 0;
    bool all_good = true;
    for (uint32_t i = 0; i < t.pieces.size(); ++i) {
        const uint64_t size = piece_size(t, i);
        for (uint32_t off = 0; off <= size; ++off) {
            for (uint32_t len = 0; off + len <= size; ++len) {
                ++reads;
                if (dm.read_block(i, off, len) != expected(i * pl + off, len)) {
                    std::printf("      mismatch: piece %u off %u len %u\n", i, off, len);
                    all_good = false;
                }
            }
        }
    }
    check(all_good, "every (piece, offset, length): " + std::to_string(reads) + " reads");
}

static void test_out_of_range() {
    const TorrentFile t = make_single("range.bin", 4, 10);   // pieces of 4, 4, 2
    TempDir dir;
    DiskManager dm(t, dir.path);

    expect_throw("read piece index == piece count", [&] { (void)dm.read_block(3, 0, 1); });
    expect_throw("read past the end of a piece",    [&] { (void)dm.read_block(0, 2, 3); });
    expect_throw("read past the short last piece",  [&] { (void)dm.read_block(2, 0, 3); });
    expect_throw("write piece index == piece count", [&] { dm.write_piece({3, {1}}); });
    expect_throw("write past the end of the torrent", [&] {
        dm.write_piece({2, std::vector<uint8_t>(4)});
    });
}

static void test_resume() {
    const TorrentFile t = make_multi("resume", 4, {3, 5, 2});
    TempDir dir;
    {
        DiskManager dm(t, dir.path);
        write_all(dm, t);
    }
    const DiskManager again(t, dir.path);
    check(reads_match(again, t), "data survives reconstructing the manager");
}

static void test_preallocation() {
    const TorrentFile t = make_multi("prealloc", 4, std::vector<FileInfo>{
        {7, {"deep", "er", "a"}}, {0, {"empty"}}, {5, {"b"}}});
    TempDir dir;
    const DiskManager dm(t, dir.path);

    const fs::path root = dir.path / "prealloc";
    check(fs::is_directory(root / "deep" / "er"), "missing directories are created");
    check(fs::file_size(root / "deep" / "er" / "a") == 7, "file preallocated to its length");
    check(fs::exists(root / "empty") && fs::file_size(root / "empty") == 0,
          "zero-length file is created empty");
    check(dm.total_length() == 12, "total_length matches the files");
}

static void test_truncated_file() {
    const TorrentFile t = make_multi("trunc", 4, {6, 2});
    TempDir dir;
    DiskManager dm(t, dir.path);
    write_all(dm, t);

    fs::resize_file(dir.path / "trunc" / "f0", 1);
    expect_throw("read after the file was truncated", [&] { (void)dm.read_block(0, 0, 4); });
}

static void test_open_failure() {
    const TorrentFile t = make_single("taken", 4, 4);
    TempDir dir;
    fs::create_directory(dir.path / "taken");   // a directory where the file should go

    try {
        DiskManager dm(t, dir.path);
        check(false, "open fails when the path is a directory");
    } catch (const std::exception& e) {
        const std::string what = e.what();
        check(what.find((dir.path / "taken").string()) != std::string::npos,
              "open error names the path -> " + what);
    }
}

static void test_empty_torrent() {
    const TorrentFile t = make_multi("empty", 4, {0, 0});
    TempDir dir;
    DiskManager dm(t, dir.path);
    check(dm.total_length() == 0 && t.pieces.empty(), "all-zero-length torrent constructs");
    dm.sync();
    expect_throw("read from a torrent with no pieces", [&] { (void)dm.read_block(0, 0, 0); });
}

int main() {
    test_layouts();
    test_exhaustive_reads();
    test_out_of_range();
    test_resume();
    test_preallocation();
    test_truncated_file();
    test_open_failure();
    test_empty_torrent();

    std::printf("\n%d failure(s)\n", failures);
    return failures != 0;
}