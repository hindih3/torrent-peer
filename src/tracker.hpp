#include <sys/socket.h>
#include <arpa/inet.h>
#include "torrent.hpp"
#include "utils.hpp"

int peerFD = createUDPIpv4Socket();
TrackerAddress address = parse_tracker_url();