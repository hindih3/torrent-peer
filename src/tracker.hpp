#pragma once
#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include "torrent.hpp"
#include "utils.hpp"

struct Peer {
    std::string ip;
    uint16_t port;
};