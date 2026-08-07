#pragma once
#include <cstdint>
#include <unistd.h>
#include <vector>

constexpr uint32_t BLOCK_SIZE = 16384;

struct BlockRequest {
    uint32_t piece_index;
    uint32_t offset;
    uint32_t length;
};

struct Block {
    uint32_t piece_index;
    uint32_t offset;
    std::vector<uint8_t> data;
};

struct CompletedPiece {
    uint32_t index;
    std::vector<uint8_t> data;
};

struct File {
    int fd = -1;

    File() = default;
    explicit File(int f) : fd(f) {}
    ~File() { if (fd >= 0) ::close(fd); }

    File(const File&)            = delete;
    File& operator=(const File&) = delete;

    File(File&& o) noexcept : fd(o.fd) { o.fd = -1; }
    File& operator=(File&& o) noexcept {
        if (this != &o) { if (fd >= 0) ::close(fd); fd = o.fd; o.fd = -1; }
        return *this;
    }
};

// one bit per piece, packed into bytes, MSB first.
// Piece 0 is bit 0x80 of byte 0. Spare bits in the last byte must be zero.
class Bitfield {
public:
    Bitfield() = default;
    explicit Bitfield(uint32_t bits)
        : bits_(bits), bytes_((bits + 7) / 8, 0) {}

    // Build from a peer's raw bitfield message. Rejects wrong-length payloads
    // and set spare bits, both of which are protocol violations worth dropping
    // the peer over.
    static Bitfield from_bytes(const std::vector<uint8_t>& raw, uint32_t bits) {
        if (raw.size() != (bits + 7) / 8) throw std::runtime_error("bitfield: wrong length");
        Bitfield bf(bits);
        bf.bytes_ = raw;
        const uint32_t spare = bf.bytes_.size() * 8 - bits;
        if (spare > 0 && (bf.bytes_.back() & ((1u << spare) - 1)) != 0) {
            throw std::runtime_error("bitfield: spare bits set");
        }
        return bf;
    }

    bool get(uint32_t i) const {
        if (i >= bits_) return false;
        return (bytes_[i / 8] >> (7 - (i % 8))) & 1u;
    }

    void set(uint32_t i) {
        if (i >= bits_) throw std::out_of_range("bitfield index");
        bytes_[i / 8] |= static_cast<uint8_t>(1u << (7 - i % 8));
    }

    uint32_t size() const { return bits_; }
    const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
    uint32_t             bits_ = 0;
    std::vector<uint8_t> bytes_;
};
