#include <iostream>
#include <fstream>
#include <sstream>
#include "parser.hpp"


int main() {
    std::ifstream file("/home/hamza/Downloads/big-buck-bunny.torrent", std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string data = buffer.str();

    Bencode_parser parser(data);
    Bencode_value value = parser.parse();
    std::cout << value;
    std::cout << '\n';
}