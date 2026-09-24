cmake_minimum_required(VERSION 3.16)
project(torrent-peer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)

add_library(torrent_peer_lib STATIC
        src/cli/args.cpp
        src/metainfo/parser.cpp
        src/metainfo/torrent.cpp
        src/runtime/session.cpp
        src/runtime/managers/disk_manager.cpp
        src/runtime/managers/peer_manager.cpp
        src/runtime/managers/piece_manager.cpp
        src/runtime/managers/tracker_manager.cpp
)
target_include_directories(torrent_peer_lib PUBLIC src)
target_link_libraries(torrent_peer_lib PRIVATE OpenSSL::Crypto)
target_compile_options(torrent_peer_lib PRIVATE -Wall -Wextra -Wpedantic)

add_executable(torrent-peer src/main.cpp)
target_link_libraries(torrent-peer PRIVATE torrent_peer_lib)

add_executable(tracker_probe tools/tracker_probe.cpp)
target_link_libraries(tracker_probe PRIVATE torrent_peer_lib)

add_executable(peer_probe tools/peer_probe.cpp)
target_link_libraries(peer_probe PRIVATE torrent_peer_lib)