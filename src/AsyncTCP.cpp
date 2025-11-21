/*
  Reimplementation of an asynchronous TCP library for Espressif MCUs, using
  BSD sockets.

  Copyright (c) 2020 Alex Villacís Lasso.

  Original AsyncTCP API Copyright (c) 2016 Hristo Gochkov. All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "AsyncTCP.h"

#include <atomic>

#include <errno.h>
#include <lwip/dns.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include "Arduino.h"  // IWYU pragma: keep
#include "lwip/sockets.h"

#undef close
#undef connect
#undef write
#undef read

#define MAX_PAYLOAD_SIZE 1360

// Since the only task reading from these sockets is the asyncTcpPSock task
// and all socket clients are serviced sequentially, only one read buffer
// is needed, and it can therefore be statically allocated
static uint8_t _readBuffer[MAX_PAYLOAD_SIZE];

AsyncClient::AsyncClient(int sockfd)
    : _connect_cb(0),
      _connect_cb_arg(0),
      _discard_cb(0),
      _discard_cb_arg(0),
      _sent_cb(0),
      _sent_cb_arg(0),
      _error_cb(0),
      _error_cb_arg(0),
      _recv_cb(0),
      _recv_cb_arg(0),
      _timeout_cb(0),
      _timeout_cb_arg(0),
      _rx_last_packet(0),
      _rx_since_timeout(0),
      _ack_timeout(ASYNC_MAX_ACK_TIME),
      _connect_port(0)
#if ASYNC_TCP_SSL_ENABLED
      ,
      _root_ca_len(0),
      _root_ca(NULL),
      _cli_cert_len(0),
      _cli_cert(NULL),
      _cli_key_len(0),
      _cli_key(NULL),
      _secure(false),
      _handshake_done(true),
      _psk_ident(0),
      _psk(0),
      _sslctx(NULL)
#endif  // ASYNC_TCP_SSL_ENABLED
      ,
      _writeSpaceRemaining(TCP_SND_BUF),
      _conn_state(0) {





/*
const char * AsyncClient::stateToString(){
    switch(state()){
        case 0: return "Closed";
        case 1: return "Listen";
        case 2: return "SYN Sent";
        case 3: return "SYN Received";
        case 4: return "Established";
        case 5: return "FIN Wait 1";
        case 6: return "FIN Wait 2";
        case 7: return "Close Wait";
        case 8: return "Closing";
        case 9: return "Last ACK";
        case 10: return "Time Wait";
        default: return "UNKNOWN";
    }
}
*/

/*
  Async TCP Server
 */

AsyncServer::AsyncServer(IPAddress addr, uint16_t port)
    : _port(port), _addr(addr), _noDelay(false), _connect_cb(0), _connect_cb_arg(0) {
}

AsyncServer::AsyncServer(uint16_t port)
    : _port(port),
      _addr((uint32_t)IPADDR_ANY),
      _noDelay(false),
      _connect_cb(0),
      _connect_cb_arg(0) {
}

AsyncServer::~AsyncServer() {
    // xSemaphoreTakeRecursive(_asyncsock_mutex, (TickType_t)portMAX_DELAY);
    end();
}

void AsyncServer::onClient(AcConnectHandler cb, void* arg) {
    _connect_cb = cb;
    _connect_cb_arg = arg;
}

void AsyncServer::begin() {
    if (isOpen())
        return;

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        return;

    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = (uint32_t)_addr;
    server.sin_port = htons(_port);
    if (bind(sockfd, (struct sockaddr*)&server, sizeof(server)) < 0) {
        ::close(sockfd);
        log_e("bind error: %d - %s", errno, strerror(errno));
        return;
    }

    static uint8_t backlog = 5;
    if (listen(sockfd, backlog) < 0) {
        ::close(sockfd);
        log_e("listen error: %d - %s", errno, strerror(errno));
        return;
    }
    fcntl(sockfd, F_SETFL, O_NONBLOCK);

    // Updating state visible to asyncTcpSock task
    _socket = sockfd;
}

void AsyncServer::end() {
    if (!isOpen())
        return;

    ::close(_socket.exchange(-1));
}

void AsyncServer::_sockIsReadable() {
    // Serial.print("AsyncServer::_sockIsReadable: "); Serial.println(_socket);

    if (_connect_cb) {
        struct sockaddr_in client;
        size_t cs = sizeof(struct sockaddr_in);
        errno = 0;
        int accepted_sockfd =
            ::accept(_socket, (struct sockaddr*)&client, (socklen_t*)&cs);
        // Serial.printf("\t new sockfd=%d errno=%d\r\n", accepted_sockfd, errno);
        if (accepted_sockfd < 0) {
            log_e("accept error: %d - %s", errno, strerror(errno));
            return;
        }

        AsyncClient* c = new AsyncClient(accepted_sockfd);
        if (c) {
            c->setNoDelay(_noDelay);
            _connect_cb(_connect_cb_arg, c);
        }
    }
}

bool AsyncServer::_sockIsWriteable() {
    // dummy impl
    return false;
}

void AsyncServer::_sockPoll() {
    // dummy impl
}

void AsyncServer::_sockDelayedConnect() {
    // dummy impl
}

bool AsyncServer::_pendingWrite() {
    // dummy impl
    return false;
}

bool AsyncServer::_isServer() {
    return true;
}