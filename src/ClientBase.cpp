#include "ClientBase.hpp"

#include <utility>

#include <esp32-hal-log.h>
#include <lwip/dns.h>
#include <lwip/sockets.h>

#include "lwip/ip_addr.h"

#ifdef EINPROGRESS
#if EINPROGRESS != 119
#error EINPROGRESS invalid
#endif
#endif

using namespace AsyncTcpSock;

// This function runs in the LWIP thread
void ClientBase::dnsFoundCallback(const char* _, const ip_addr_t* ip, void* arg) {
    ClientBase* c = static_cast<ClientBase*>(arg);

    if (ip) {
        c->_ip.from_ip_addr_t(ip);
    } else {
        c->_ip = IPAddress();
    }

    c->_isdnsfinished = true;

    // TODO: actually use name
}

bool ClientBase::connect(IPAddress ip, std::uint16_t port) {
    if (isOpen()) {
        log_w("already connected, state %d", std::to_underlying(_state));
        return false;
    }

#if ASYNC_TCP_SSL_ENABLED
    _secure = secure;
    _handshake_done = !secure;
#endif  // ASYNC_TCP_SSL_ENABLED

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        log_e("socket: %d", errno);
        return false;
    }

    int r = fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);

    sockaddr serveraddr = std::bit_cast<sockaddr>(sockaddr_in{.sin_len = 0,
                                                              .sin_family = AF_INET,
                                                              .sin_port = htons(port),
                                                              .sin_addr = {.s_addr = ip},
                                                              .sin_zero = {}});

    // Serial.printf("DEBUG: connect to %08x port %d using IP... ", ip_addr, port);
    errno = 0;
    r = ::connect(sockfd, &serveraddr, sizeof(serveraddr));
    // Serial.printf("r=%d errno=%d\r\n", r, errno);
    if (r < 0 && errno != EINPROGRESS) {
        // Serial.println("\t(connect failed)");
        log_e("connect on fd %d, errno: %d, \"%s\"", sockfd, errno, strerror(errno));
        ::close(sockfd);
        return false;
    }

    _ip = ip;
    _port = port;
    _state = ConnectionState::CONNECTED;
    _rx_last_packet = std::chrono::steady_clock::now();

    // Updating state visible to asyncTcpSock task
    _socket = sockfd;

    // Socket is now connecting. Should become writable in asyncTcpSock task
    // Serial.printf("\twaiting for connect finished on socket: %d\r\n", _socket);
    return true;
}

bool ClientBase::connect(const char* host, std::uint16_t port) {
    log_v("connect to %s port %d using DNS...", host, port);

    ip_addr_t addr;
    err_t err = dns_gethostbyname(host, &addr, &dnsFoundCallback, this);

    if (err == ERR_OK) {
        IPAddress resolved(&addr);
        log_v("\taddr resolved as %s, connecting...", resolved.toString().c_str());

#if ASYNC_TCP_SSL_ENABLED
        _hostname = host;
        return connect(IPAddress(addr.u_addr.ip4.addr), port, secure);
#else
        return connect(std::move(resolved), port);
#endif  // ASYNC_TCP_SSL_ENABLED

    } else if (err == ERR_INPROGRESS) {
        log_v("\twaiting for DNS resolution");
        _state = ConnectionState::WAITING_FOR_DNS;
        _port = port;

#if ASYNC_TCP_SSL_ENABLED
        _hostname = host;
        _secure = secure;
        _handshake_done = !secure;
#endif  // ASYNC_TCP_SSL_ENABLED

        return true;
    }

    log_e("error: %d", err);
    return false;
}
