#pragma once
#include <string>
#include <vector>
#include <iomanip>
#include "parser.hpp"
#include "utils.hpp"

struct Peer {
    std::string ip;
    uint16_t port;
};

struct TorrentFile {
    std::string announce;
    std::string info_hash;
    std::string name;
    size_t piece_length;
    std::vector<std::string> pieces;
    size_t total_length;
    std::string peer_id;

    struct File {
        size_t length;
        std::string path;
    };
    std::vector<File> files;
};

std::vector<std::string> generate_pieces(const Bencode_value::Dict& info) {
    std::vector<std::string> pieces;
    const std::string& raw_pieces = info.at("pieces").get_string();
    for (size_t i = 0; i < raw_pieces.size(); i += 20) {
        pieces.push_back(raw_pieces.substr(i, 20));
    }
    return pieces;
}

TorrentFile parse_torrent(const std::string& data) {
    TorrentFile torrent;
    Bencode_parser parser(data);
    Bencode_value val = parser.parse();

    const auto& dict = val.get_dict();
    const auto& info = dict.at("info").get_dict();

    torrent.announce = dict.at("announce").get_string();
    torrent.name = info.at("name").get_string();
    torrent.piece_length = info.at("piece length").get_int();
    torrent.pieces = generate_pieces(info);
    
    torrent.total_length = 0;
    if (info.count("files"))  {
        for (const auto& file : info.at("files").get_list())
            torrent.total_length += file.get_dict().at("length").get_int();
    } else {
        torrent.total_length = info.at("length").get_int();
    }

    auto [info_start, info_end] = parser.get_info_range();
    torrent.info_hash = sha1(data, info_start, info_end - info_start);
    torrent.peer_id = generate_peer_id();

    return torrent;
}

void print_torrent(const TorrentFile& torrent) {
    std::cout << "Name:         " << torrent.name << "\n";
    std::cout << "Announce:     " << torrent.announce << "\n";
    std::cout << "Total length: " << torrent.total_length << " bytes\n";
    std::cout << "Piece length: " << torrent.piece_length << " bytes\n";
    std::cout << "Pieces:       " << torrent.pieces.size() << "\n";
    std::cout << "Info hash:    ";
    for (unsigned char c : torrent.info_hash)
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)c;
    std::cout << std::dec << "\n";
}