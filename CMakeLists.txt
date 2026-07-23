cmake_minimum_required(VERSION 3.16)
project(torrent-peer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)

add_executable(torrent-peer
        src/main.cpp
        src/peer.cpp
        src/session.hpp
        src/torrent.cpp
        src/tracker.cpp
)

target_include_directories(torrent-peer PRIVATE src)
target_link_libraries(torrent-peer PRIVATE OpenSSL::Crypto)