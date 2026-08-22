#include "disk_manager.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace {
fs::path safe_join(const fs::path& root, const std::vector<std::string>& components) {
    fs::path p = root;
    for (const auto& c : components) {
        if (c.empty() || c == "." || c == ".." ||
            c.find('/') != std::string::npos || c.find('\\') != std::string::npos) {
            throw std::runtime_error("unsafe path component in torrent: '" + c + "'");
        }
        p /= c;
    }
    return p;
}

void reserve(int fd, uint64_t length, const fs::path& path) {
#if defined(__linux__)
    if (length > 0) {
        int rc = ::posix_fallocate(fd, 0, static_cast<off_t>(length));
        if (rc == 0) return;
        if (rc != EOPNOTSUPP && rc != EINVAL && rc != ENOSYS) {
            throw_errno("posix_fallocate " + path.string(), rc);
        }
    }
#endif
    // posix_fallocate offers a strong guarantee that the full memory will be reserved
    // ftruncate only creates a sparse file; blocks are only allocated when written to
    // with ftruncuate, a download will fail hours in if the disk space gets full when writing

    if (ftruncate(fd, static_cast<off_t>(length)) < 0) {
        throw_errno("ftruncate " + path.string());
    }
}

void pwrite_all(int fd, const uint8_t* buf, uint64_t len, uint64_t offset) {
    uint64_t done = 0;
    while (done < len) {
        ssize_t n = ::pwrite(fd, buf + done, len - done,
                             static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_errno("pwrite");
        }

        // should never happen, but protects against infinite looping
        if (n == 0) throw std::runtime_error("pwrite wrote 0 bytes");
        done += static_cast<uint64_t>(n);
    }
}

void pread_all(int fd, uint8_t* buf, uint64_t len, uint64_t offset) {
    uint64_t done = 0;
    while (done < len) {
        ssize_t n = ::pread(fd, buf + done, len - done,
                            static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_errno("pread");
        }

        // 0 means EOF, either the file is truncated/corrupted or the args are wrong
        if (n == 0) throw std::runtime_error("unexpected EOF while reading");
        done += static_cast<uint64_t>(n);
    }
}

}

DiskManager::DiskManager(const TorrentFile& torrent, const fs::path& download_dir)
    : piece_length_(static_cast<uint64_t>(torrent.piece_length)),
      total_length_(torrent.total_length),
      piece_count_(static_cast<uint32_t>(torrent.pieces.size())) {

    if (piece_length_ == 0) throw std::runtime_error("torrent has piece_length 0");

    std::vector<std::pair<fs::path, uint64_t>> layout;
    if (is_multifile(torrent)) {
        const fs::path root = safe_join(download_dir, {torrent.name});
        for (const FileInfo& f : *torrent.files) {
            if (f.length < 0) throw std::runtime_error("negative file length");
            layout.emplace_back(safe_join(root, f.path), static_cast<uint64_t>(f.length));
        }
    } else {
        layout.emplace_back(safe_join(download_dir, {torrent.name}),
                            static_cast<uint64_t>(torrent.length.value_or(0)));
    }

    uint64_t cursor = 0;
    files_.reserve(layout.size());
    for (auto& [path, length] : layout) {
        if (path.has_parent_path()) fs::create_directories(path.parent_path());

        // 0644: owner can read and write
        //       everyone else can only read
        int fd = open(path.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd < 0) throw_errno("open " + path.string());

        FileEntry entry;
        entry.file   = File(fd);
        entry.path   = path;
        entry.offset = cursor;
        entry.length = length;

        reserve(fd, length, path);
        cursor += length;

        if (length > 0) files_.push_back(std::move(entry));
    }

    if (cursor != total_length_) {
        throw std::runtime_error("file lengths (" + std::to_string(cursor) +
                                 ") do not sum to total_length (" +
                                 std::to_string(total_length_) + ")");
    }
}

// returns index of first FileEntry behind an offset
size_t DiskManager::locate(const uint64_t offset) const {

    // contrary to what I initially thought, std::lower_bound finds the first element that is
    // greater than or equal to the value, so upper_bound is plainly simpler and less bug-prone
    auto it = std::upper_bound(files_.begin(), files_.end(), offset,
                               [](uint64_t value, const FileEntry& e) { return value < e.offset; });
    if (it == files_.begin()) throw std::out_of_range("offset before start of torrent");
    return static_cast<size_t>(std::prev(it) - files_.begin());
}

template <typename Op>
void DiskManager::for_each_slice(uint64_t global_offset, uint64_t len, Op op) const {
    if (len == 0) return;
    if (global_offset + len > total_length_) {
        throw std::out_of_range("range extends past the end of the torrent");
    }

    size_t index  = locate(global_offset);
    uint64_t done = 0;
    while (done < len) {
        const FileEntry& f = files_[index];
        const uint64_t file_off = global_offset + done - f.offset;
        const uint64_t n = std::min(len - done, f.length - file_off);

        op(f.file.fd, done, n, file_off);

        done += n;
        ++index;
    }
}

void DiskManager::write_piece(const CompletedPiece& piece) {
    if (piece.index >= piece_count_) throw std::out_of_range("piece index out of range");

    const uint64_t global_offset = static_cast<uint64_t>(piece.index) * piece_length_;
    const uint8_t* src = piece.data.data();

    for_each_slice(global_offset, piece.data.size(),
                   [src](int fd, uint64_t buf_pos, uint64_t n, uint64_t file_off) {
                       pwrite_all(fd, src + buf_pos, n, file_off);
                   });
}

std::vector<uint8_t> DiskManager::read_block(uint32_t piece_index, uint32_t offset,
                                             uint32_t length) const {
    if (piece_index >= piece_count_) throw std::out_of_range("piece index out of range");

    const uint64_t this_piece_len =
        (piece_index == piece_count_ - 1)
            ? total_length_ - static_cast<uint64_t>(piece_index) * piece_length_
            : piece_length_;

    if (static_cast<uint64_t>(offset) + length > this_piece_len)
        throw std::out_of_range("read_block range extends past the piece");

    const uint64_t global_offset =
        static_cast<uint64_t>(piece_index) * piece_length_ + offset;

    std::vector<uint8_t> out(length);
    uint8_t* dst = out.data();

    for_each_slice(global_offset, length,
                   [dst](int fd, uint64_t buf_pos, uint64_t n, uint64_t file_off) {
                       pread_all(fd, dst + buf_pos, n, file_off);
                   });
    return out;
}

//flush kernel buffer
void DiskManager::sync() const {
    for (const FileEntry& f : files_) {
        if (::fsync(f.file.fd) < 0) throw_errno("fsync " + f.path.string());
    }
}