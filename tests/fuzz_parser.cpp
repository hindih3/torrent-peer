#include "metainfo/parser.hpp"
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string input(reinterpret_cast<const char*>(data), size);
    try {
        BencodeParser parser(input);
        parser.parse();
    } catch (const std::runtime_error&) {
        // rejected input is fine
    }
    return 0;
}