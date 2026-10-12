#include "metainfo/torrent.hpp"
#include "core/sha1.hpp"

#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// parse_torrent is the gate between untrusted bytes and the rest of the client.
// Every input here is valid bencode (that's test_parser's job), so anything
// rejected is rejected for describing a torrent that can't exist.

static int failures = 0;

static void check(const bool good, const std::string& label) {
    std::printf("%s  %s\n", good ? "ok  " : "FAIL", label.c_str());
    if (!good) ++failures;
}

static void expect_reject(const std::string& label, const std::string& data) {
    try {
        (void)parse_torrent(data);
        std::printf("FAIL  expected reject: %s\n", label.c_str());
        ++failures;
    } catch (const std::exception& e) {
        std::printf("ok    %s -> %s\n", label.c_str(), e.what());
    }
}

// Runs the checks only if the torrent parses, so one bad parse doesn't hide
// behind a crash in the checks that follow it.
static void expect_accept(const std::string& label, const std::string& data,
                          const std::function<void(const TorrentFile&)>& checks = {}) {
    try {
        const TorrentFile t = parse_torrent(data);
        std::printf("ok    %s\n", label.c_str());
        if (checks) checks(t);
    } catch (const std::exception& e) {
        std::printf("FAIL  expected accept: %s -> %s\n", label.c_str(), e.what());
        ++failures;
    }
}

// --- bencode builders --------------------------------------------------------

// Key -> already-encoded value. std::map keeps keys in raw byte order, which is
// the order the parser insists on.
using Entries = std::map<std::string, std::string>;

static std::string str(const std::string& s) { return std::to_string(s.size()) + ":" + s; }
static std::string num(const int64_t v)      { return "i" + std::to_string(v) + "e"; }

static std::string list(const std::vector<std::string>& encoded) {
    std::string out = "l";
    for (const auto& e : encoded) out += e;
    return out + "e";
}

static std::string strings(const std::vector<std::string>& raw) {
    std::vector<std::string> encoded;
    for (const auto& s : raw) encoded.push_back(str(s));
    return list(encoded);
}

static std::string dict(const Entries& d) {
    std::string out = "d";
    for (const auto& [k, v] : d) out += str(k) + v;
    return out + "e";
}

// --- torrent builders --------------------------------------------------------

constexpr int64_t kPl        = 16384;
constexpr int64_t kMaxPl     = 64 * 1024 * 1024;   // torrent.cpp's kPieceLengthCap
constexpr int64_t kMax       = std::numeric_limits<int64_t>::max();
constexpr const char* kAnnounce = "udp://tracker.example:6969/announce";

static std::string pieces(const uint64_t count) { return str(std::string(count * 20, 'p')); }

static uint64_t piece_count(const uint64_t total, const int64_t pl) {
    const auto p = static_cast<uint64_t>(pl);
    return total / p + (total % p != 0);
}

static Entries single_info(const std::string& name = "file.bin", const int64_t length = 40000,
                           const int64_t pl = kPl) {
    const uint64_t total = length > 0 ? static_cast<uint64_t>(length) : 0;
    return {{"name",         str(name)},
            {"piece length", num(pl)},
            {"length",       num(length)},
            {"pieces",       pieces(pl > 0 ? piece_count(total, pl) : 1)}};
}

struct F {
    int64_t                  length;
    std::vector<std::string> path;
};

// The piece count is computed the way a naive parser would: negative lengths
// are skipped and the sum wraps. So a test that should be rejected for its
// lengths always has a matching piece count, and can't pass by tripping the
// piece count check instead.
static Entries multi_info(const std::vector<F>& files, const std::string& name = "dir",
                          const int64_t pl = kPl) {
    std::vector<std::string> encoded;
    uint64_t total = 0;
    for (const auto& [length, path] : files) {
        encoded.push_back(dict({{"length", num(length)}, {"path", strings(path)}}));
        if (length > 0) total += static_cast<uint64_t>(length);
    }
    return {{"name",         str(name)},
            {"piece length", num(pl)},
            {"files",        list(encoded)},
            {"pieces",       pieces(piece_count(total, pl))}};
}

static std::string torrent(const Entries& info, Entries top = {{"announce", str(kAnnounce)}}) {
    top["info"] = dict(info);
    return dict(top);
}

static Entries without(Entries d, const std::string& key) {
    d.erase(key);
    return d;
}

static Entries with(Entries d, const std::string& key, const std::string& encoded) {
    d[key] = encoded;
    return d;
}

// --- accepted torrents -------------------------------------------------------

static void test_single_file() {
    expect_accept("single file", torrent(single_info("file.bin", 40000)), [](const TorrentFile& t) {
        check(t.name == "file.bin",                    "  name");
        check(t.piece_length == kPl,                   "  piece length");
        check(t.length == 40000 && !t.files,           "  length set, files unset");
        check(!is_multifile(t),                        "  not multi-file");
        check(t.total_length == 40000,                 "  total_length");
        check(t.pieces.size() == 3,                    "  3 pieces");
        check(t.pieces[0] == std::string(20, 'p'),     "  piece hashes split into 20 bytes");
        check(piece_size(t, 0) == kPl,                 "  full first piece");
        check(piece_size(t, 2) == 40000 - 2 * kPl,     "  short last piece");
    });

    expect_accept("length an exact multiple of piece length",
                  torrent(single_info("even.bin", 2 * kPl)), [](const TorrentFile& t) {
        check(t.pieces.size() == 2,         "  2 pieces");
        check(piece_size(t, 1) == kPl,      "  last piece is full");
    });

    expect_accept("piece length at the cap", torrent(single_info("cap.bin", 10, kMaxPl)),
                  [](const TorrentFile& t) {
        check(t.pieces.size() == 1 && piece_size(t, 0) == 10, "  one 10-byte piece");
    });
}

static void test_multi_file() {
    const auto info = multi_info({{100, {"a.txt"}},
    {0, {"empty"}}, {50000, {"sub", "b.bin"}}}, "pack");
    expect_accept("multi file", torrent(info), [](const TorrentFile& t) {
        check(t.name == "pack",                         "  name");
        check(is_multifile(t) && !t.length,             "  files set, length unset");
        check(t.files->size() == 3,                     "  3 files");
        check((*t.files)[0].length == 100 &&
              (*t.files)[0].path == std::vector<std::string>{"a.txt"}, "  first file");
        check((*t.files)[1].length == 0,                "  zero-length file kept");
        check((*t.files)[2].path == std::vector<std::string>{"sub", "b.bin"}, "  nested path");
        check(t.total_length == 50100,                  "  total_length is the sum");
        check(t.pieces.size() == 4,                     "  4 pieces");
    });

    // DiskManager's empty-torrent test relies on this being accepted.
    expect_accept("every file zero-length", torrent(multi_info({{0, {"a"}},
    {0, {"b"}}})), [](const TorrentFile& t) {
        check(t.total_length == 0 && t.pieces.empty(), "  no pieces");
    });
}

static void test_trackers() {
    expect_accept("announce only", torrent(single_info()), [](const TorrentFile& t) {
        check(t.announce == kAnnounce, "  announce");
        check(t.announce_list == std::vector<std::vector<std::string>>{{kAnnounce}},
              "  announce becomes the only tier");
    });

    const std::string tiers = list({strings({"udp://a/1", "udp://b/1"}), strings({"udp://c/2"})});
    expect_accept("announce-list",
                  torrent(single_info(), {{"announce", str(kAnnounce)}, {"announce-list", tiers}}),
                  [](const TorrentFile& t) {
        check(t.announce_list == std::vector<std::vector<std::string>>{
                  {"udp://a/1", "udp://b/1"}, {"udp://c/2"}},
              "  tiers kept in order; announce not added");
    });

    expect_accept("no trackers at all", torrent(single_info(), {}), [](const TorrentFile& t) {
        check(t.announce.empty() && t.announce_list.empty(), "  both empty");
    });

    expect_accept("url-list",
                  torrent(single_info(), {{"url-list", strings({"http://a/", "http://b/"})}}),
                  [](const TorrentFile& t) {
        check(t.url_list == std::vector<std::string>{"http://a/", "http://b/"}, "  web seeds");
    });
}

static void test_optional_fields() {
    const Entries top = {{"announce",      str(kAnnounce)},
                         {"comment",       str("hello")},
                         {"created by",    str("tester 1.0")},
                         {"creation date", num(1700000000)},
                         {"encoding",      str("UTF-8")}};
    expect_accept("optional fields present", torrent(single_info(), top), [](const TorrentFile& t) {
        check(t.comment == "hello",               "  comment");
        check(t.created_by == "tester 1.0",       "  created by");
        check(t.creation_date == 1700000000,      "  creation date");
        check(t.encoding == "UTF-8",              "  encoding");
    });

    expect_accept("optional fields absent", torrent(single_info()), [](const TorrentFile& t) {
        check(!t.comment && !t.created_by && !t.creation_date && !t.encoding, "  all unset");
    });

    expect_accept("unknown keys ignored",
                  torrent(with(single_info(), "private", num(1)),
                          {{"announce", str(kAnnounce)}, {"x-custom", list({num(1), str("x")})}}));
}

// The info hash identifies the torrent to trackers and peers, so it has to be
// the SHA-1 of the info dict's exact bytes, including keys we don't use.
static void test_info_hash() {
    const Entries info = with(single_info(), "private", num(1));
    const std::string data = torrent(info);
    const std::string info_bytes = dict(info);
    const size_t start = data.find(info_bytes);

    expect_accept("info hash", data, [&](const TorrentFile& t) {
        check(start != std::string::npos && t.info_hash == sha1(data, start, info_bytes.size()),
              "  SHA-1 of the raw info dict");
    });

    try {
        const auto base      = parse_torrent(torrent(single_info()));
        const auto with_key  = parse_torrent(torrent(info));
        const auto commented = parse_torrent(torrent(single_info(),
                                   {{"announce", str(kAnnounce)}, {"comment", str("x")}}));
        check(base.info_hash != with_key.info_hash, "unknown info key changes the hash");
        check(base.info_hash == commented.info_hash, "keys outside info don't change the hash");
    } catch (const std::exception& e) {
        check(false, std::string("info hash comparisons -> ") + e.what());
    }
}

// --- rejected torrents -------------------------------------------------------

static void test_structure() {
    expect_reject("root is a list",         list({dict({{"info", dict(single_info())}})}));
    expect_reject("root is an integer",     num(1));
    expect_reject("missing info",           dict({{"announce", str(kAnnounce)}}));
    expect_reject("info is a string",       dict({{"info", str("nope")}}));
    expect_reject("info is a list",         dict({{"info", list({})}}));

    expect_reject("missing name",           torrent(without(single_info(), "name")));
    expect_reject("missing piece length",   torrent(without(single_info(), "piece length")));
    expect_reject("missing pieces",         torrent(without(single_info(), "pieces")));
    expect_reject("name is an integer",     torrent(with(single_info(), "name", num(1))));
    expect_reject("piece length is a string", torrent(with(single_info(), "piece length", str("16384"))));
    expect_reject("length is a string",     torrent(with(single_info(), "length", str("40000"))));
    expect_reject("files is a dict",        torrent(with(multi_info({{1, {"a"}}}), "files", dict({}))));

    expect_reject("both length and files",
                  torrent(with(multi_info({{40000, {"a"}}}), "length", num(40000))));
    expect_reject("neither length nor files", torrent(without(single_info(), "length")));

    expect_reject("announce-list is a string",
                  torrent(single_info(), {{"announce-list", str(kAnnounce)}}));
    expect_reject("announce-list tier is a string",
                  torrent(single_info(), {{"announce-list", list({str(kAnnounce)})}}));
}

static void test_files_structure() {
    auto with_files = [](const std::vector<std::string>& encoded_files) {
        return torrent(with(multi_info({{1, {"a"}}}), "files", list(encoded_files)));
    };
    expect_reject("file entry is a string",  with_files({str("a")}));
    expect_reject("file missing length",     with_files({dict({{"path", strings({"a"})}})}));
    expect_reject("file missing path",       with_files({dict({{"length", num(1)}})}));
    expect_reject("path is a string",        with_files({dict({{"length", num(1)}, {"path", str("a")}})}));
    expect_reject("path element is an integer",
                  with_files({dict({{"length", num(1)}, {"path", list({num(1)})}})}));
    expect_reject("empty path list",         torrent(multi_info({{1, {}}})));
}

static void test_lengths() {
    expect_reject("negative length",              torrent(single_info("neg.bin", -1)));
    expect_reject("negative file length",         torrent(multi_info({{4, {"a"}}, {-1, {"b"}}, {4, {"c"}}})));
    expect_reject("piece length 0",               torrent(single_info("p.bin", 4, 0)));
    expect_reject("negative piece length",        torrent(single_info("p.bin", 4, -16384)));
    expect_reject("piece length over the cap",    torrent(single_info("p.bin", 4, kMaxPl + 1)));

    // Two max-size files plus 18 bytes wrap a uint64_t total around to 16,
    // and the piece count matches that wrapped total, so only an overflow
    // check can catch it.
    expect_reject("file lengths overflow the total",
                  torrent(multi_info({{kMax, {"a"}}, {kMax, {"b"}}, {18, {"c"}}})));
}

static void test_pieces() {
    const Entries info = single_info("p.bin", 40000);   // needs 3 pieces
    expect_reject("pieces not a multiple of 20",   torrent(with(info, "pieces", str(std::string(61, 'p')))));
    expect_reject("one piece too few",             torrent(with(info, "pieces", pieces(2))));
    expect_reject("one piece too many",            torrent(with(info, "pieces", pieces(4))));
    expect_reject("no pieces for a non-empty file", torrent(with(info, "pieces", str(""))));
}

// Path safety is checked here first; DiskManager's safe_join stays as a backstop.
// The NUL case matters because open() stops at the first NUL, so "a\0b" and "a"
// would silently become the same file.
static void test_unsafe_paths() {
    const std::vector<std::pair<std::string, std::string>> bad = {
        {"..", "'..'"}, {".", "'.'"}, {"", "empty"}, {"a/b", "'a/b'"},
        {"a\\b", "'a\\b'"}, {std::string("a\0b", 3), "embedded NUL"},
    };
    for (const auto& [name, shown] : bad) {
        expect_reject("single-file name " + shown,  torrent(single_info(name)));
        expect_reject("multi-file name " + shown,   torrent(multi_info({{1, {"ok"}}}, name)));
        expect_reject("file name " + shown,         torrent(multi_info({{1, {"ok", name}}})));
        expect_reject("directory name " + shown,    torrent(multi_info({{1, {name, "ok"}}})));
    }
}

// Two entries with the same path map different parts of the torrent onto the
// same bytes, and a file can't also be a directory. Both describe a layout no
// filesystem can hold.
static void test_path_conflicts() {
    expect_reject("duplicate paths, not adjacent",
                  torrent(multi_info({{6, {"a"}}, {2, {"b"}}, {6, {"a"}}})));
    expect_reject("duplicate nested paths",
                  torrent(multi_info({{1, {"d", "x"}}, {1, {"d", "x"}}})));
    expect_reject("file 'x' then 'x/y'",
                  torrent(multi_info({{2, {"x"}}, {2, {"x", "y"}}})));
    expect_reject("file 'x/y' then 'x'",
                  torrent(multi_info({{2, {"x", "y"}}, {2, {"x"}}})));
    expect_reject("file 'a/b' then 'a/b/c'",
                  torrent(multi_info({{1, {"a", "b"}}, {1, {"a", "b", "c"}}})));

    // Look like conflicts to a check that compares strings instead of path components.
    expect_accept("siblings in one directory",torrent(multi_info({{1, {"d", "x"}}, {1, {"d", "y"}}})));
    expect_accept("'a' and 'ab/c'",           torrent(multi_info({{1, {"a"}}, {1, {"ab", "c"}}})));
    expect_accept("'a' and 'a.txt'",          torrent(multi_info({{1, {"a"}}, {1, {"a.txt"}}})));
    expect_accept("'a/b' and 'b/a'",          torrent(multi_info({{1, {"a", "b"}}, {1, {"b", "a"}}})));
}

int main() {
    test_single_file();
    test_multi_file();
    test_trackers();
    test_optional_fields();
    test_info_hash();
    test_structure();
    test_files_structure();
    test_lengths();
    test_pieces();
    test_unsafe_paths();
    test_path_conflicts();

    std::printf("\n%d failure(s)\n", failures);
    return failures != 0;
}
