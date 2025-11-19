#include "ClientBase.hpp"

#include <cerrno>
#include <chrono>
#include <type_traits>
#include <utility>

#include <esp32-hal-log.h>
#include <lwip/dns.h>
#include <lwip/sockets.h>

#include "Callbacks.hpp"
#include "WriteQueueBuffer.hpp"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"

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

    // Updating state visible to asyncTcpSock task
    _socket = sockfd;

    // Socket is now connecting. Should become writable in asyncTcpSock task, which then
    // updates the state in _sockIsWritable().
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

void ClientBase::close(bool _) {
    if (isOpen())
        _close();
}

err_enum_t ClientBase::abort() {
    if (isOpen()) {
        // Note: needs LWIP_SO_LINGER to be enabled in order to work, otherwise
        // this call is equivalent to close().
        linger l{.l_onoff = 1, .l_linger = 0};
        setsockopt(_socket, SOL_SOCKET, SO_LINGER, &l, sizeof(l));

        _close();
    }

    return ERR_ABRT;
}

bool ClientBase::freeable() const {
    if (!isOpen()) {
        return true;
    }

    return _state == ConnectionState::DISCONNECTED;
}

bool ClientBase::connected() const {
    return _state == ConnectionState::CONNECTED;
}

bool ClientBase::canSend() const {
    return space() > 0;
}

std::size_t ClientBase::space() const {
    if (!connected())
        return 0;

    return _writeSpaceRemaining;
}

void ClientBase::_close() {
    _state = ConnectionState::DISCONNECTED;
    ::close(_socket.exchange(-1));

    _clearWriteQueue();

    _callbacks.invoke(CallbackType::DISCONNECT);
}

void ClientBase::_error(int errorCode) {
    _close();
    // TODO: Callback order switched wrt original impl. Issue?
    _callbacks.invoke(CallbackType::ERROR, errorCode);
}

bool ClientBase::_processWriteQueue(std::unique_lock<std::mutex>&) {
    // Assume we can write to the socket, calling this otherwise makes no sense.
    // Also assume, that _writeMutex is locked.

    bool activity = false;
    for (auto& buf : _writeQueue) {
        // Early bailout if this buffer already has an error for some reason
        if (WriteQueueBufferUtil::hasError(buf)) {
            break;
        }

        // Skip fully written buffers
        if (WriteQueueBufferUtil::isFullyWritten(buf)) {
            continue;
        }

        std::size_t written = WriteQueueBufferUtil::write(buf, _socket);
        _writeSpaceRemaining += written;
        activity = written > 0;
    }

    return activity;
}

void ClientBase::_cleanupWriteQueue(std::unique_lock<std::mutex>& lock) {
    // Assume that _writeMutex is locked.

    std::vector<WriteStats> notifyQueue;
    notifyQueue.reserve(_writeQueue.size());

    // Check front of queue for finished buffers and collect some stats about them.
    std::size_t toRemove = 0;
    for (const auto& buf : _writeQueue) {
        if (WriteQueueBufferUtil::hasError(buf)) {
            std::visit([&](auto&& it) { _error(it.errorCode); }, buf);
            break;
        }

        if (!WriteQueueBufferUtil::isFullyWritten(buf)) {
            // Buffer is not fully written, stop.
            break;
        }

        std::visit(
            [&](auto&& it) {
                if (it.writtenAt > _rx_last_packet) {
                    _rx_last_packet = it.writtenAt;
                }

                notifyQueue.emplace_back(WriteStats{
                    it.amountWritten,
                    std::chrono::duration_cast<
                        std::chrono::duration<std::uint32_t, std::milli>>(it.writtenAt -
                                                                          it.queuedAt)});
            },
            buf);
        ++toRemove;
    }

    _writeQueue.erase(_writeQueue.begin(), _writeQueue.begin() + toRemove);

    // Unlock before we call any callbacks to avoid issues
    lock.unlock();

    for (const WriteStats& stats : notifyQueue) {
        _callbacks.invoke(CallbackType::SENT, stats.length, stats.delay.count());
    }
}

void ClientBase::_clearWriteQueue() {
    std::lock_guard lock(_writeMutex);
    _writeQueue.clear();
    _writeSpaceRemaining = INITIAL_WRITE_SPACE;
}

bool ClientBase::_checkAckTimeout() {
    if (_ack_timeout_signaled || !_ack_timeout)
        // Handler already called or no timeout set, continue normally.
        return false;

    std::unique_lock lock(_writeMutex);

    if (!_writeQueue.empty()) {
        // Check the first element in the queue to see how long it's been waiting to be sent.

        const auto& first = WriteQueueBufferUtil::asCommonView(_writeQueue.front());
        auto delay = std::chrono::steady_clock::now() - first.queuedAt;

        if (delay >= *_ack_timeout &&
            first.writtenAt == std::chrono::steady_clock::time_point{}) {
            // ACK timed out and nothing was ever written
            _ack_timeout_signaled = true;

            lock.unlock();
            _callbacks.invoke(CallbackType::TIMEOUT, delay);

            return true;
        }
    }

    return false;
}

bool ClientBase::_checkRxTimeout() {
    if (!_rx_timeout)
        return false;

    const auto now = std::chrono::steady_clock::now();
    if (now - _rx_last_packet < _rx_timeout)
        return false;

    {
        // Check if this socket can even be read. It may be the case that the select(...)
        // call in the manager task fails because any of the sockets is not readable or
        // writable, but we only want to time out if we are the problematic socket.

        fd_set sockSet_r{};
        timeval timeout{};
        FD_ZERO(&sockSet_r);
        FD_SET(_socket, &sockSet_r);

        int selected = select(_socket + 1, &sockSet_r, nullptr, nullptr, &timeout);
        if (selected > 0) {
            // This socket is still readable. Reset timeout.
            _rx_last_packet = now;
            return false;
        }
    }

    // This connection has timed out and the socket is no longer readable. The connection
    // should be closed shortly and be disposed of.
    return true;
}

bool ClientBase::_sockIsWriteable() {
    bool activity = false;

    // Socket is now writeable. What should we do?
    if (_state != ConnectionState::CONNECTED) {
        // Socket has finished connecting, check status
        socklen_t socketErrorSize = sizeof(int);
        int socketError = 0;
        int result =
            getsockopt(_socket, SOL_SOCKET, SO_ERROR, &socketError, &socketErrorSize);

        if (result < 0) {
            _error(errno);
            return false;
        } else if (socketError != 0) {
            _error(socketError);
            return false;
        }

        activity = true;

        _state = ConnectionState::CONNECTED;
        _rx_last_packet = std::chrono::steady_clock::now();
        _ack_timeout_signaled = false;
        _callbacks.invoke(CallbackType::CONNECT, this);
    }

    {
        std::unique_lock lock(_writeMutex);
        if (_state == ConnectionState::CONNECTED && _writeQueue.size() > 0) {
            // We are connected. Write available data.
            activity = _processWriteQueue(lock);
            _cleanupWriteQueue(lock);
        }
    }

    return activity;
}

void ClientBase::_sockIsReadable() {
    errno = 0;

    ssize_t result =
        lwip_read(_socket, SHARED_READ_BUFFER.data(), SHARED_READ_BUFFER.size());

    if (result > 0) {
        _rx_last_packet = std::chrono::steady_clock::now();
        // result contains the amount of data read
        _callbacks.invoke(CallbackType::RECV, SHARED_READ_BUFFER.data(), result);
    } else if (result == 0) {
        // A successful read of 0 bytes indicates that the remote side closed the
        // connection
        _close();
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        // Errors other than these should be handled
        _error(errno);
    }
}

void ClientBase::_sockDelayedConnect() {
    if (_ip) {
        connect(_ip, _port);
    } else {
        _error(ERR_DNS_RESOLUTION_FAILED);
    }
}

void ClientBase::_sockPoll() {
    if (!connected())
        return;

    {
        // send() may be invoked from threads other than the manager task, causing the
        // write queue to be processed without notifications being sent. Do this now.
        std::unique_lock lock(_writeMutex);
        _cleanupWriteQueue(lock);
    }

    // Processing notifications may have disconnected us
    if (!connected())
        return;

    if (_checkAckTimeout()) {
        // An ACK timeout will not yet close the connection
        return;
    }

    if (_checkRxTimeout()) {
        // A receive timeout will close the connection if the socket is no more readable
        _close();
        return;
    }

    _callbacks.invoke(CallbackType::POLL);
}
