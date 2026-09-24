#include "args.hpp"

#include <charconv>
#include <format>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

#include "core/log.hpp"

namespace {
    [[noreturn]] void usage_error(const std::string& msg) {
        throw std::invalid_argument(msg);
    }

    uint16_t parse_port(std::string_view s) {
        unsigned v = 0;
        auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        if (ec != std::errc{} || p != s.data() + s.size() || v < 1024 || v > 65535)
            usage_error(std::format("invalid port: {}", s));
        return static_cast<uint16_t>(v);
    }

    Peer parse_peer(std::string_view s) {
        const size_t colon = s.rfind(':');
        if (colon == std::string_view::npos || colon == 0 || colon + 1 == s.size())
            usage_error(std::format("--peer wants host:port, got: {}", s));
        return {.host = std::string(s.substr(0, colon)), .port = std::string(s.substr(colon + 1))};
    }
}

args parse_args(int argc, char* argv[]) {
    args args;
    std::vector<std::string_view> positional;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto value = [&] {
            if (++i >= argc) usage_error(std::format("{} needs a value", a));
            return std::string_view(argv[i]);
        };

        if      (a == "--port")       args.port = parse_port(value());
        else if (a == "--peer")       args.manual_peers.push_back(parse_peer(value()));
        else if (a == "--no-tracker") args.use_tracker = false;
        else if (a == "--log-level") {
            const auto v = value();
            if (!set_log_level(v)) usage_error(std::format("unknown log level: {}", v));
        }
        else if (a.starts_with('-'))  usage_error(std::format("unknown option: {}", a));
        else                          positional.push_back(a);
    }

    if (positional.empty() || positional.size() > 2)
        usage_error("expected <file.torrent> [download-dir]");

    args.torrent_path = positional[0];
    if (positional.size() == 2) args.out_dir = positional[1];
    return args;
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error(std::format("could not open: {}", p.string()));
    return {std::istreambuf_iterator<char>(f), {}};
}