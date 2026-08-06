cmake_minimum_required(VERSION 3.16)
project(torrent-peer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)

add_executable(torrent-peer
        src/main.cpp
        src/net/peer.cpp
        src/net/session.hpp
        src/bencode/torrent.cpp
        src/net/tracker.cpp
        src/net/session.cpp
        src/bencode/parser.cpp
        src/download/piece_manager.cpp
        src/download/piece_manager.hpp
        src/download/common.hpp
        src/download/disk_manager.cpp
        src/download/disk_manager.hpp
        src/download/peer_manager.cpp
        src/download/peer_manager.hpp
)

target_include_directories(torrent-peer PRIVATE src)
target_link_libraries(torrent-peer PRIVATE OpenSSL::Crypto)