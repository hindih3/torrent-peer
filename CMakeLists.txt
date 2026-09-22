cmake_minimum_required(VERSION 3.16)
project(torrent-peer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)

add_executable(torrent-peer
        src/main.cpp
        src/net/session.hpp
        src/bencode/torrent.cpp
        src/net/session.cpp
        src/bencode/parser.cpp
        src/download/piece_manager.cpp
        src/download/piece_manager.hpp
        src/download/common.hpp
        src/download/disk_manager.cpp
        src/download/disk_manager.hpp
        src/download/peer_manager.cpp
        src/download/peer_manager.hpp
        src/download/tracker_manager.cpp
        src/download/tracker_manager.hpp
)
target_include_directories(torrent-peer PRIVATE src)
target_link_libraries(torrent-peer PRIVATE OpenSSL::Crypto)

add_executable(tracker_probe
        src/tools/tracker_probe.cpp
        src/download/tracker_manager.cpp
        src/bencode/parser.cpp
        src/bencode/torrent.cpp
)
target_include_directories(tracker_probe PRIVATE src)
target_link_libraries(tracker_probe PRIVATE OpenSSL::Crypto)

add_executable(peer_probe
        src/tools/peer_probe.cpp
        src/download/peer_manager.hpp
        src/download/peer_manager.cpp
        src/bencode/utils.hpp
        src/bencode/torrent.hpp
        src/bencode/torrent.cpp
        src/bencode/parser.cpp
)
target_include_directories(peer_probe PRIVATE src)
target_link_libraries(peer_probe PRIVATE OpenSSL::Crypto)