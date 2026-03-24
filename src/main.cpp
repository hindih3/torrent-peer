#include <iostream>
#include <fstream>
#include <sstream>
#include "parser.hpp"
#include "torrent.hpp"
#include "utils.hpp"

int main() {
    std::string file_name;
    std::cin >> file_name;
    
    std::ifstream file(file_name, std::ios::binary);
    if (!file)
        throw std::runtime_error("Could not open file");

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string data = buffer.str();

    Bencode_parser parser(data);
    Bencode_value value = parser.parse();

    TorrentFile torrent = parse_torrent(data);
    print_torrent(torrent);
}