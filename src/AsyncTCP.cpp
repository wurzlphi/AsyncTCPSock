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

    if (sockfd != -1) {
        fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);

        // Updating state visible to asyncTcpSock task
        // xSemaphoreTakeRecursive(_asyncsock_mutex, (TickType_t)portMAX_DELAY);
        _conn_state = 4;
        _socket = sockfd;
        _rx_last_packet = millis();
        // xSemaphoreGiveRecursive(_asyncsock_mutex);
    }
}

AsyncClient::~AsyncClient() {
    if (isOpen())
        _close();

    _removeAllCallbacks();
}

void AsyncClient::setRxTimeout(uint32_t timeout) {
    _rx_since_timeout = timeout;
}

uint32_t AsyncClient::getRxTimeout() {
    return _rx_since_timeout;
}

uint32_t AsyncClient::getAckTimeout() {
    return _ack_timeout;
}

void AsyncClient::setAckTimeout(uint32_t timeout) {
    _ack_timeout = timeout;
}

void AsyncClient::setNoDelay(bool nodelay) {
    if (!isOpen())
        return;

    int flag = nodelay;
    int res = setsockopt(_socket, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));
    if (res < 0) {
        log_e("fail on fd %d, errno: %d, \"%s\"", _socket.load(), errno, strerror(errno));
    }
}

bool AsyncClient::getNoDelay() {
    if (!isOpen())
        return false;

    int flag = 0;
    socklen_t size = sizeof(int);
    int res = getsockopt(_socket, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, &size);
    if (res < 0) {
        log_e("fail on fd %d, errno: %d, \"%s\"", _socket.load(), errno, strerror(errno));
    }
    return flag;
}

/*
 * Callback Setters
 * */

void AsyncClient::onConnect(AcConnectHandler cb, void* arg) {
    _connect_cb = cb;
    _connect_cb_arg = arg;
}

void AsyncClient::onDisconnect(AcConnectHandler cb, void* arg) {
    _discard_cb = cb;
    _discard_cb_arg = arg;
}

void AsyncClient::onAck(AcAckHandler cb, void* arg) {
    _sent_cb = cb;
    _sent_cb_arg = arg;
}

void AsyncClient::onError(AcErrorHandler cb, void* arg) {
    _error_cb = cb;
    _error_cb_arg = arg;
}

void AsyncClient::onData(AcDataHandler cb, void* arg) {
    _recv_cb = cb;
    _recv_cb_arg = arg;
}

void AsyncClient::onTimeout(AcTimeoutHandler cb, void* arg) {
    _timeout_cb = cb;
    _timeout_cb_arg = arg;
}

void AsyncClient::onPoll(AcConnectHandler cb, void* arg) {
    _poll_cb = cb;
    _poll_cb_arg = arg;
}

uint32_t AsyncClient::getRemoteAddress() {
    if (!isOpen()) {
        return 0;
    }

    struct sockaddr_storage addr;
    socklen_t len = sizeof addr;
    getpeername(_socket, (struct sockaddr*)&addr, &len);
    struct sockaddr_in* s = (struct sockaddr_in*)&addr;

    return s->sin_addr.s_addr;
}

uint16_t AsyncClient::getRemotePort() {
    if (!isOpen()) {
        return 0;
    }

    struct sockaddr_storage addr;
    socklen_t len = sizeof addr;
    getpeername(_socket, (struct sockaddr*)&addr, &len);
    struct sockaddr_in* s = (struct sockaddr_in*)&addr;

    return ntohs(s->sin_port);
}

uint32_t AsyncClient::getLocalAddress() {
    if (!isOpen()) {
        return 0;
    }

    struct sockaddr_storage addr;
    socklen_t len = sizeof addr;
    getsockname(_socket, (struct sockaddr*)&addr, &len);
    struct sockaddr_in* s = (struct sockaddr_in*)&addr;

    return s->sin_addr.s_addr;
}

uint16_t AsyncClient::getLocalPort() {
    if (!isOpen()) {
        return 0;
    }

    struct sockaddr_storage addr;
    socklen_t len = sizeof addr;
    getsockname(_socket, (struct sockaddr*)&addr, &len);
    struct sockaddr_in* s = (struct sockaddr_in*)&addr;

    return ntohs(s->sin_port);
}

IPAddress AsyncClient::remoteIP() {
    return IPAddress(getRemoteAddress());
}

uint16_t AsyncClient::remotePort() {
    return getRemotePort();
}

IPAddress AsyncClient::localIP() {
    return IPAddress(getLocalAddress());
}

uint16_t AsyncClient::localPort() {
    return getLocalPort();
}

#if ASYNC_TCP_SSL_ENABLED
int AsyncClient::_runSSLHandshakeLoop() {
    int res = 0;

    while (!_handshake_done) {
        res = _sslctx->runSSLHandshake();
        if (res == 0) {
            // Handshake successful
            _handshake_done = true;
        } else if (ASYNCTCP_TLS_CAN_RETRY(res)) {
            // Ran out of readable data or writable space on socket, must continue later
            break;
        } else {
            // SSL handshake for AsyncTCP does not inform SSL errors
            log_e("TLS setup failed with error %d, closing socket...", res);
            _close();
            // _sslctx should be NULL after this
            break;
        }
    }

    return res;
}
#endif

void AsyncClient::_sockPoll() {
    if (!connected())
        return;

    // The AsyncClient::send() call may be invoked from tasks other than "asyncTcpSock"
    // and may have written buffers via _flushWriteQueue(), but the ack callbacks have
    // not been called yet, nor buffers removed from the write queue. For consistency,
    // written buffers are now acked here.
    std::deque<notify_writebuf> notifylist;
    int sent_errno = 0;

    {
        std::lock_guard lock(writeMutex);
        if (_writeQueue.size() > 0) {
            _collectNotifyWrittenBuffers(notifylist, sent_errno);
        }
    }
    _notifyWrittenBuffers(notifylist, sent_errno);
    if (!connected())
        return;

    uint32_t now = millis();

    // ACK Timeout - simulated by write queue staleness
    {
        std::unique_lock lock(writeMutex);

        if (_writeQueue.size() > 0 && !_ack_timeout_signaled && _ack_timeout) {
            uint32_t sent_delay = now - _writeQueue.front().queued_at;
            if (sent_delay >= _ack_timeout && _writeQueue.front().written_at == 0) {
                _ack_timeout_signaled = true;
                // log_w("ack timeout %d", pcb->state);
                lock.unlock();
                if (_timeout_cb)
                    _timeout_cb(_timeout_cb_arg, this, sent_delay);
                return;
            }
        }
    }

    // RX Timeout? Check for readable socket before bailing out
    if (_rx_since_timeout && (now - _rx_last_packet) >= (_rx_since_timeout * 1000)) {
        fd_set sockSet_r;
        struct timeval tv;

        FD_ZERO(&sockSet_r);
        FD_SET(_socket, &sockSet_r);
        tv.tv_sec = 0;
        tv.tv_usec = 0;

        int r = select(_socket + 1, &sockSet_r, NULL, NULL, &tv);
        if (r > 0)
            _rx_last_packet = now;
    }

    // RX Timeout
    if (_rx_since_timeout && (now - _rx_last_packet) >= (_rx_since_timeout * 1000)) {
        // log_w("rx timeout %d", pcb->state);
        _close();
        return;
    }
    // Everything is fine
    if (_poll_cb) {
        _poll_cb(_poll_cb_arg, this);
    }
}

void AsyncClient::_removeAllCallbacks() {
    _connect_cb = NULL;
    _connect_cb_arg = NULL;
    _discard_cb = NULL;
    _discard_cb_arg = NULL;
    _sent_cb = NULL;
    _sent_cb_arg = NULL;
    _error_cb = NULL;
    _error_cb_arg = NULL;
    _recv_cb = NULL;
    _recv_cb_arg = NULL;
    _timeout_cb = NULL;
    _timeout_cb_arg = NULL;
    _poll_cb = NULL;
    _poll_cb_arg = NULL;
}

size_t AsyncClient::add(const char* data, size_t size, uint8_t apiflags) {
    queued_writebuf n_entry;

    if (!connected() || data == NULL || size <= 0)
        return 0;

    size_t room = space();
    if (!room)
        return 0;

    size_t will_send = (room < size) ? room : size;
    if (apiflags & ASYNC_WRITE_FLAG_COPY) {
        n_entry.data = (uint8_t*)malloc(will_send);
        if (n_entry.data == NULL) {
            return 0;
        }
        memcpy(n_entry.data, data, will_send);
        n_entry.owned = true;
    } else {
        n_entry.data = (uint8_t*)data;
        n_entry.owned = false;
    }
    n_entry.length = will_send;
    n_entry.written = 0;
    n_entry.queued_at = millis();
    n_entry.written_at = 0;
    n_entry.write_errno = 0;

    {
        std::lock_guard lock(writeMutex);

        _writeQueue.push_back(n_entry);
        _writeSpaceRemaining -= will_send;
        _ack_timeout_signaled = false;
    }

    return will_send;
}

bool AsyncClient::send() {
    if (!connected())
        return false;

    fd_set sockSet_w;
    struct timeval tv;

    FD_ZERO(&sockSet_w);
    FD_SET(_socket, &sockSet_w);
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    // Write as much data as possible from queue if socket is writable
    std::lock_guard lock(writeMutex);
    int ready = select(_socket + 1, NULL, &sockSet_w, NULL, &tv);
    if (ready > 0)
        _flushWriteQueue();
    return true;
}

bool AsyncClient::_pendingWrite() {
    std::lock_guard lock(writeMutex);
    return (_conn_state > 0 && _conn_state < 4) || _writeQueue.size() > 0;
}

bool AsyncClient::_isServer() {
    return false;
}

size_t AsyncClient::write(const char* data) {
    if (data == NULL) {
        return 0;
    }
    return write(data, strlen(data));
}

size_t AsyncClient::write(const char* data, size_t size, uint8_t apiflags) {
    size_t will_send = add(data, size, apiflags);
    if (!will_send || !send()) {
        return 0;
    }
    return will_send;
}

#if ASYNC_TCP_SSL_ENABLED
void AsyncClient::setRootCa(const char* rootca, const size_t len) {
    _root_ca = (char*)rootca;
    _root_ca_len = len;
}

void AsyncClient::setClientCert(const char* cli_cert, const size_t len) {
    _cli_cert = (char*)cli_cert;
    _cli_cert_len = len;
}

void AsyncClient::setClientKey(const char* cli_key, const size_t len) {
    _cli_key = (char*)cli_key;
    _cli_key_len = len;
}

void AsyncClient::setPsk(const char* psk_ident, const char* psk) {
    _psk_ident = psk_ident;
    _psk = psk;
}
#endif  // ASYNC_TCP_SSL_ENABLED

const char* AsyncClient::errorToString(int8_t error) {
    switch (error) {
        case ERR_OK:
            return "OK";
        case ERR_MEM:
            return "Out of memory error";
        case ERR_BUF:
            return "Buffer error";
        case ERR_TIMEOUT:
            return "Timeout";
        case ERR_RTE:
            return "Routing problem";
        case ERR_INPROGRESS:
            return "Operation in progress";
        case ERR_VAL:
            return "Illegal value";
        case ERR_WOULDBLOCK:
            return "Operation would block";
        case ERR_USE:
            return "Address in use";
        case ERR_ALREADY:
            return "Already connected";
        case ERR_CONN:
            return "Not connected";
        case ERR_IF:
            return "Low-level netif error";
        case ERR_ABRT:
            return "Connection aborted";
        case ERR_RST:
            return "Connection reset";
        case ERR_CLSD:
            return "Connection closed";
        case ERR_ARG:
            return "Illegal argument";
        case -55:
            return "DNS failed";
        default:
            return "UNKNOWN";
    }
}
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