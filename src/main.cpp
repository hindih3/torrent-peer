#include <iostream>
#include <fstream>
#include <sstream>
#include "parser.hpp"
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
    print_value(value);

    auto [info_start, info_end] = parser.get_info_range();
    std::string hash = sha1(data, info_start, info_end - info_start);

    std::cout << "info hash: ";
    for (unsigned char c : hash)
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)c;
    std::cout << '\n';
}