cmake_minimum_required(VERSION 3.16)
project(torrent-peer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

find_package(OpenSSL REQUIRED)

option(TP_SANITIZE "Enable ASan/UBSan" OFF)
option(TP_ASSERTIONS "Enable libstdc++ assertions" OFF)
option(TP_BUILD_TESTS "Build unit and regression tests" ON)
option(TP_FUZZ "Build libFuzzer harness (clang only)" OFF)

if(TP_SANITIZE)
    add_compile_options(
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
    )
    add_link_options(-fsanitize=address,undefined)
endif()

if(TP_ASSERTIONS)
    add_compile_options(-D_GLIBCXX_ASSERTIONS)
endif()

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

add_executable(tracker-probe tools/tracker_probe.cpp)
target_link_libraries(tracker-probe PRIVATE torrent_peer_lib)

add_executable(peer-probe tools/peer_probe.cpp)
target_link_libraries(peer-probe PRIVATE torrent_peer_lib)

if(TP_BUILD_TESTS)
    enable_testing()

    add_executable(test-parser tests/test_parser.cpp)
    target_link_libraries(test-parser PRIVATE torrent_peer_lib)
    add_test(NAME parser COMMAND test-parser)
endif()

if(TP_FUZZ)
    add_executable(fuzz-parser tests/fuzz_parser.cpp src/metainfo/parser.cpp)
    target_include_directories(fuzz-parser PRIVATE src)
    target_compile_options(fuzz-parser PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz-parser PRIVATE -fsanitize=fuzzer,address,undefined)
endif()