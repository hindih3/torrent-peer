#pragma once
#include "parser.hpp"
#include "utils.hpp"
#include <optional>
#include <iostream>
#include <iomanip>

struct FileInfo {
    int64_t length;
    std::vector<std::string> path;
};

struct TorrentFile {

    // tracker
    std::string announce;
    std::vector<std::vector<std::string>> announce_list;
    std::vector<std::string> url_list;

    // informational field
    std::optional<std::string> comment;
    std::optional<std::string> created_by;
    std::optional<int64_t> creation_date;
    std::optional<std::string> encoding;

    // info dict
    std::string name;
    int64_t piece_length;
    std::vector<std::string> pieces;
    std::array<uint8_t, 20> info_hash;

    // single-file
    std::optional<int64_t> length;

    // multi-file
    std::optional<std::vector<FileInfo>> files;

    uint64_t total_length;
};

bool is_multifile(const TorrentFile& t) {
    return t.files.has_value();
}

std::vector<std::string> generate_pieces(const Bencode_value::Dict& info) {
    std::vector<std::string> pieces;
    const std::string& raw_pieces = info.at("pieces").get_string();
    for (size_t i = 0; i < raw_pieces.size(); i += 20) {
        pieces.push_back(raw_pieces.substr(i, 20));
    }
    return pieces;
}

uint64_t piece_size(const TorrentFile& t, size_t piece_index) {
    if (piece_index == t.pieces.size() - 1)
        return t.total_length - (piece_index * t.piece_length);
    return t.piece_length;
}

uint64_t calculate_total_length(const TorrentFile& t) {
    if (t.length.has_value())
        return t.length.value();
    uint64_t total = 0;
    for (const auto& f : t.files.value())
        total += f.length;
    return total;
}

TorrentFile parse_torrent(const std::string& data) {
    TorrentFile torrent;
    Bencode_parser parser(data);
    Bencode_value root = parser.parse();

    const Bencode_value::Dict& dict = root.get_dict();
    const Bencode_value::Dict& info = dict.at("info").get_dict();

    if (auto it = dict.find("announce"); it != dict.end())
        torrent.announce = it->second.get_string();
    if (auto it = dict.find("announce-list"); it != dict.end()) {
        for (const auto& tier : it->second.get_list()) {
            std::vector<std::string> t;
            for (const auto& url : tier.get_list())
                t.push_back(url.get_string());
            torrent.announce_list.push_back(std::move(t));
        }
    }

    if (auto it = dict.find("url-list"); it != dict.end())
        for (const auto& url : it->second.get_list())
            torrent.url_list.push_back(url.get_string());

    if (auto it = info.find("length"); it != info.end())
        torrent.length = it->second.get_int();

    if (auto it = info.find("files"); it != info.end()) {
        std::vector<FileInfo> files;
        for (const auto& f : it->second.get_list()) {
            FileInfo file;
            file.length = f.get_dict().at("length").get_int();
            for (const auto& p : f.get_dict().at("path").get_list())
                file.path.push_back(p.get_string());
            files.push_back(std::move(file));
        }
        torrent.files = std::move(files);
    }

    if (auto it = dict.find("comment"); it != dict.end())
        torrent.comment = it->second.get_string();
    if (auto it = dict.find("created by"); it != dict.end())
        torrent.created_by = it->second.get_string();
    if (auto it = dict.find("creation date"); it != dict.end())
        torrent.creation_date = it->second.get_int();
    if (auto it = dict.find("encoding"); it != dict.end())
        torrent.encoding = it->second.get_string();

    torrent.name = info.at("name").get_string();
    torrent.piece_length = info.at("piece length").get_int();
    torrent.pieces = generate_pieces(info);

    auto [info_start, info_end] = parser.get_info_range();
    torrent.info_hash = sha1(data, info_start, info_end - info_start);

    torrent.total_length = calculate_total_length(torrent);

    return torrent;
}


void print_torrent(const TorrentFile& t, bool flag = false) {
    std::cout << "Name: " << t.name << '\n';
    std::cout << "Announce: " << t.announce << '\n';
    std::cout << "Piece length: " << t.piece_length << '\n';
    std::cout << "Number of pieces: " << t.pieces.size() << '\n';

    if (t.length) std::cout << "Length: " << *t.length << '\n';
    else std::cout << "Length: (none)\n";

    if (t.files && flag == true) {
        std::cout << "Files:\n";
        for (const auto& f : *t.files) {
            std::cout << "  - path: ";
            for (const auto& p : f.path)
                std::cout << p << "/";
            std::cout << " length: " << f.length << '\n';
        }
    } else {
        std::cout << "Files: (none / not shown)\n";
    }

    if (t.comment) std::cout << "Comment: " << *t.comment << '\n';
    else std::cout << "Comment: (none)\n";

    if (t.created_by) std::cout << "Created by: " << *t.created_by << '\n';
    else std::cout << "Created by: (none)\n";

    if (t.creation_date) std::cout << "Creation date: " << *t.creation_date << '\n';
    else std::cout << "Creation date: (none)\n";

    if (t.encoding) std::cout << "Encoding: " << *t.encoding << '\n';
    else std::cout << "Encoding: (none)\n";

    std::cout << "Announce list:\n";
    if (t.announce_list.empty()) {
        std::cout << "  (none)\n";
    } else {
        for (const auto& tier : t.announce_list) {
            std::cout << "  tier: ";
            for (const auto& url : tier)
                std::cout << url << " ";
            std::cout << '\n';
        }
    }

    std::cout << "URL list:\n";
    if (t.url_list.empty()) {
        std::cout << "  (none)\n";
    } else {
        for (const auto& url : t.url_list)
            std::cout << "  " << url << '\n';
    }

    std::cout << "Info hash: ";
    for (uint8_t b : t.info_hash)
        std::cout << std::hex << (int)b;

    std::cout << std::dec << '\n';
    std::cout << "Total length: " << t.total_length << '\n';
}