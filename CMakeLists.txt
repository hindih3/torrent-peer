cmake_minimum_required(VERSION 3.10)
project(bencode_parser)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include_directories(include)

add_executable(bencode_parser
    src/main.cpp
    src/parser.hpp
)