<<<<<<< HEAD
#include <sys/socket.h>
#include <arpa/inet.h>
#include "torrent.hpp"
#include "utils.hpp"

int peerFD = createUDPIpv4Socket();
TrackerAddress address = parse_tracker_url();
=======
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
>>>>>>> refs/remotes/origin/main
