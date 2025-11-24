#ifndef ASYNCTCPSOCK_CLIENTBASE_TPP
#define ASYNCTCPSOCK_CLIENTBASE_TPP

#include "ClientBase.hpp"

//

#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

#include <esp32-hal-log.h>
#include <lwip/dns.h>
#include <lwip/ip_addr.h>
#include <lwip/sockets.h>

#include "Callbacks.hpp"
#include "WriteQueueBuffer.hpp"

#ifdef EINPROGRESS
#if EINPROGRESS != 119
#error EINPROGRESS invalid
#endif
#endif

namespace AsyncTcpSock {

// This function runs in the LWIP thread
template <class Client>
void ClientBase<Client>::dnsFoundCallback(const char* _, const ip_addr_t* ip, void* arg) {
    ClientBase* c = static_cast<ClientBase*>(arg);

    if (ip) {
        c->_ip.from_ip_addr_t(ip);
    } else {
        c->_ip = IPAddress();
    }

    c->_isdnsfinished = true;

    // TODO: actually use name
}

template <class Client>
ClientBase<Client>::ClientBase()
    : SocketConnection(false) {
}

template <class Client>
ClientBase<Client>::ClientBase(int socket)
    : SocketConnection(socket, false) {
    if (_socket > 0) {
        _state = ConnectionState::CONNECTED;
        _rx_last_packet = std::chrono::steady_clock::now();
    }
}

template <class Client>
ClientBase<Client>::~ClientBase() {
    close();
}

template <class Client>
bool ClientBase<Client>::connect(IPAddress ip, std::uint16_t port) {
    if (isOpen()) {
        log_w("already connected, state %d", std::to_underlying(_state));
        return false;
    }

    int socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket < 0) {
        log_e("socket() error: %d", errno);
        return false;
    }

    sockaddr serveraddr = std::bit_cast<sockaddr>(sockaddr_in{.sin_len = 0,
                                                              .sin_family = AF_INET,
                                                              .sin_port = htons(port),
                                                              .sin_addr = {.s_addr = ip},
                                                              .sin_zero = {}});

    // Serial.printf("DEBUG: connect to %08x port %d using IP... ", ip_addr, port);
    errno = 0;
    int result = ::connect(socket, &serveraddr, sizeof(serveraddr));
    // Serial.printf("r=%d errno=%d\r\n", r, errno);
    if (result < 0 && errno != EINPROGRESS) {
        // Serial.println("\t(connect failed)");
        log_e("connect on fd %d, errno: %d, \"%s\"", socket, errno, strerror(errno));
        ::close(socket);
        return false;
    }

    _ip = ip;
    _port = port;

    // Updating state visible to asyncTcpSock task
    _setSocket(socket);

    // Socket is now connecting. Should become writable in asyncTcpSock task, which then
    // updates the state in _sockIsWritable().
    return true;
}

template <class Client>
bool ClientBase<Client>::connect(const char* host, std::uint16_t port) {
    log_v("connect to %s port %d using DNS...", host, port);

    ip_addr_t addr;
    err_t err = dns_gethostbyname(host, &addr, &dnsFoundCallback, this);

    if (err == ERR_OK) {
        IPAddress resolved(&addr);
        log_v("\taddr resolved as %s, connecting...", resolved.toString().c_str());

        return connect(std::move(resolved), port);

    } else if (err == ERR_INPROGRESS) {
        log_v("\twaiting for DNS resolution");
        _state = ConnectionState::WAITING_FOR_DNS;
        _port = port;

        return true;
    }

    log_e("error: %d", err);
    return false;
}

template <class Client>
void ClientBase<Client>::close(bool _) {
    if (isOpen())
        _close();
}

template <class Client>
err_enum_t ClientBase<Client>::abort() {
    if (isOpen()) {
        // Note: needs LWIP_SO_LINGER to be enabled in order to work, otherwise
        // this call is equivalent to close().
        linger l{.l_onoff = 1, .l_linger = 0};
        setsockopt(_socket, SOL_SOCKET, SO_LINGER, &l, sizeof(l));

        _close();
    }

    return ERR_ABRT;
}

template <class Client>
bool ClientBase<Client>::freeable() const {
    if (!isOpen()) {
        return true;
    }

    return _state == ConnectionState::DISCONNECTED;
}

template <class Client>
bool ClientBase<Client>::connected() const {
    return _state == ConnectionState::CONNECTED;
}

template <class Client>
bool ClientBase<Client>::canSend() const {
    return space() > 0;
}

template <class Client>
std::size_t ClientBase<Client>::space() const {
    if (!connected())
        return 0;

    return _writeSpaceRemaining;
}

template <class Client>
std::size_t ClientBase<Client>::add(std::span<const std::uint8_t> data,
                                    ClientApiFlags apiflags) {
    return add(data.data(), data.size(), apiflags);
}

template <class Client>
std::size_t ClientBase<Client>::add(const std::uint8_t* data,
                                    std::size_t size,
                                    ClientApiFlags apiFlags) {
    if (!connected() || data == nullptr || size == 0)
        return 0;

    const std::size_t remainingSpace = space();
    if (remainingSpace == 0)
        return 0;

    const std::size_t toSend = std::min(remainingSpace, size);
    WriteQueueBuffer buf;
    if (apiFlags.test(std::to_underlying(ClientApiFlag::COPY))) {
        buf.emplace<OwnedWriteQueueBuffer>(OwnedWriteQueueBuffer{
            {.queuedAt = std::chrono::steady_clock::now()},
            std::vector<std::uint8_t>(data, data + toSend),
        });
    } else {
        buf.emplace<BorrowedWriteQueueBuffer>(BorrowedWriteQueueBuffer{
            {.queuedAt = std::chrono::steady_clock::now()},
            std::span<const std::uint8_t>(data, toSend),
        });
    }

    {
        std::lock_guard lock(_writeMutex);
        _writeQueue.push_back(std::move(buf));
        _writeSpaceRemaining -= toSend;
        _ack_timeout_signaled = false;
    }

    return toSend;
}

template <class Client>
std::size_t ClientBase<Client>::add(const char* str,
                                    std::size_t size,
                                    ClientApiFlags apiFlags) {
    if (str == nullptr)
        return 0;

    if (size == 0) {
        size = std::strlen(str);
    }

    return add(reinterpret_cast<const std::uint8_t*>(str), size, apiFlags);
}

template <class Client>
bool ClientBase<Client>::send() {
    if (!connected())
        return false;

    fd_set sockSet_w{};
    FD_ZERO(&sockSet_w);
    FD_SET(_socket, &sockSet_w);
    timeval tv{.tv_sec = 0, .tv_usec = 0};

    int ready = select(_socket + 1, nullptr, &sockSet_w, nullptr, &tv);
    if (ready > 0) {
        return _sockIsWriteable();
    }

    return false;
}

template <class Client>
std::size_t ClientBase<Client>::write(const char* str, ClientApiFlags apiFlags) {
    if (str == nullptr)
        return 0;

    return write(reinterpret_cast<const std::uint8_t*>(str), std::strlen(str), apiFlags);
}

template <class Client>
std::size_t ClientBase<Client>::write(const std::uint8_t* bytes,
                                      std::size_t size,
                                      ClientApiFlags apiFlags) {
    std::size_t toSend = add(bytes, size, apiFlags);

    if (toSend == 0) {
        // Nothing to send => nothing was written
        return 0;
    }

    if (!send()) {
        // Sending failed => nothing was written
        return 0;
    }

    return toSend;
}

template <class Client>
void ClientBase<Client>::setNoDelay(bool nodelay) {
    if (!isOpen())
        return;

    errno = 0;
    int res = setsockopt(_socket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(bool));
    if (res < 0) {
        log_e("fail on fd %d, errno: %d, \"%s\"", _socket.load(), errno, strerror(errno));
    }
}

template <class Client>
bool ClientBase<Client>::getNoDelay() {
    if (!isOpen())
        // Nagle's algorithm is enabled by default
        return false;

    bool nodelay = false;
    socklen_t size = sizeof(bool);
    errno = 0;
    int res = getsockopt(_socket, IPPROTO_TCP, TCP_NODELAY, &nodelay, &size);
    if (res < 0) {
        log_e("fail on fd %d, errno: %d, \"%s\"", _socket.load(), errno, strerror(errno));
    }

    return nodelay;
}

template <class Client>
void ClientBase<Client>::setAckTimeout(
    std::optional<std::chrono::steady_clock::duration> timeout) {
    _ack_timeout = timeout;
}

template <class Client>
void ClientBase<Client>::setRxTimeout(
    std::optional<std::chrono::steady_clock::duration> timeout) {
    _rx_timeout = timeout;
}

template <class Client>
void ClientBase<Client>::_close() {
    _state = ConnectionState::DISCONNECTED;
    ::close(_socket.exchange(-1));

    _clearWriteQueue();

    _callbacks.template invoke<CallbackType::DISCONNECT>();
}

template <class Client>
void ClientBase<Client>::_error(int errorCode) {
    // The disconnect callback may delete this client, therefore _close() has to be the
    // last operation.
    _callbacks.template invoke<CallbackType::ERROR>(errorCode);
    _close();
}

template <class Client>
bool ClientBase<Client>::_processWriteQueue(std::unique_lock<std::mutex>&) {
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

template <class Client>
void ClientBase<Client>::_cleanupWriteQueue(std::unique_lock<std::mutex>& lock) {
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
        _callbacks.template invoke<CallbackType::SENT>(stats.length, stats.delay.count());
    }
}

template <class Client>
void ClientBase<Client>::_clearWriteQueue() {
    std::lock_guard lock(_writeMutex);
    _writeQueue.clear();
    _writeSpaceRemaining = INITIAL_WRITE_SPACE;
}

template <class Client>
bool ClientBase<Client>::_checkAckTimeout() {
    if (_ack_timeout_signaled || !_ack_timeout)
        // Handler already called or no timeout set, continue normally.
        return false;

    std::unique_lock lock(_writeMutex);

    if (!_writeQueue.empty()) {
        // Check the first element in the queue to see how long it's been waiting to be
        // sent.

        const auto& first = WriteQueueBufferUtil::asCommonView(_writeQueue.front());
        auto delay = std::chrono::steady_clock::now() - first.queuedAt;

        if (delay >= *_ack_timeout &&
            first.writtenAt == std::chrono::steady_clock::time_point{}) {
            // ACK timed out and nothing was ever written
            _ack_timeout_signaled = true;

            lock.unlock();
            _callbacks.template invoke<CallbackType::TIMEOUT>(
                std::chrono::duration_cast<std::chrono::milliseconds>(delay).count());

            return true;
        }
    }

    return false;
}

template <class Client>
bool ClientBase<Client>::_checkRxTimeout() {
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

template <class Client>
bool ClientBase<Client>::_pendingWrite() {
    // Mark this socket as eligible for write polling if it is not fully connected yet or
    // if there is something in the queue, regardless of connection state.
    return (_state != ConnectionState::DISCONNECTED &&
            _state != ConnectionState::CONNECTED) ||
           [&]() {
               std::lock_guard lock(_writeMutex);
               return !_writeQueue.empty();
           }();
}

template <class Client>
bool ClientBase<Client>::_sockIsWriteable() {
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
        _callbacks.template invoke<CallbackType::CONNECT>();
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

template <class Client>
void ClientBase<Client>::_sockIsReadable() {
    errno = 0;

    ssize_t result =
        lwip_read(_socket, SHARED_READ_BUFFER.data(), SHARED_READ_BUFFER.size());

    if (result > 0) {
        _rx_last_packet = std::chrono::steady_clock::now();
        // result contains the amount of data read
        _callbacks.template invoke<CallbackType::RECV>(SHARED_READ_BUFFER.data(), result);
    } else if (result == 0) {
        // A successful read of 0 bytes indicates that the remote side closed the
        // connection
        _close();
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        // Errors other than these should be handled
        _error(errno);
    }
}

template <class Client>
void ClientBase<Client>::_sockDelayedConnect() {
    if (_ip) {
        connect(_ip, _port);
    } else {
        _error(ERR_DNS_RESOLUTION_FAILED);
    }
}

template <class Client>
void ClientBase<Client>::_sockPoll() {
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
        // _close() may have deleted this client, don't do anything more
        return;
    }

    _callbacks.template invoke<CallbackType::POLL>();
}

}  // namespace AsyncTcpSock

#endif