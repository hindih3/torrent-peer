cmake_minimum_required(VERSION 3.10)
project(torrent-peer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(OpenSSL REQUIRED)

include_directories(include)
add_executable(parser
    src/main.cpp
        src/main.cpp
        src/main.cpp
        src/peer.hpp
)

target_link_libraries(parser OpenSSL::Crypto)