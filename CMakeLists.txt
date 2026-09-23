cmake_minimum_required(VERSION 3.16)
project(torrent-peer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)

add_executable(torrent-peer
        src/main.cpp
        src/metainfo/parser.cpp
        src/metainfo/torrent.cpp
        src/runtime/session.cpp
        src/runtime/managers/disk_manager.cpp
        src/runtime/managers/peer_manager.cpp
        src/runtime/managers/piece_manager.cpp
        src/runtime/managers/tracker_manager.cpp
)
target_include_directories(torrent-peer PRIVATE src)
target_link_libraries(torrent-peer PRIVATE OpenSSL::Crypto)

add_executable(tracker_probe
        tools/tracker_probe.cpp
        src/metainfo/parser.cpp
        src/metainfo/torrent.cpp
        src/runtime/managers/tracker_manager.cpp
)
target_include_directories(tracker_probe PRIVATE src)
target_link_libraries(tracker_probe PRIVATE OpenSSL::Crypto)

add_executable(peer_probe
        tools/peer_probe.cpp
        src/metainfo/parser.cpp
        src/metainfo/torrent.cpp
        src/runtime/managers/peer_manager.cpp
)
target_include_directories(peer_probe PRIVATE src)
target_link_libraries(peer_probe PRIVATE OpenSSL::Crypto)