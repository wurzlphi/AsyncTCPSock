#include "Client.hpp"

#include <cstring>
#include <type_traits>

using namespace AsyncTcpSock;

template <
    class Getter,
    class FuncIPv4,
    class FuncIPv6,
    class Result = std::common_type_t<std::invoke_result_t<FuncIPv4, sockaddr_in&>,
                                      std::invoke_result_t<FuncIPv6, sockaddr_in6&>>>
constexpr Result withSockAddr(Getter&& getter, FuncIPv4&& useIpv4, FuncIPv6 useIpv6) {
    // I think this is the safest way to use this API...
    static_assert(sizeof(sockaddr_storage) >= sizeof(sockaddr_in6) &&
                      sizeof(sockaddr_storage) >= sizeof(sockaddr_in),
                  "sockaddr_storage too small");
    static_assert(alignof(sockaddr_storage) >= alignof(sockaddr_in6) &&
                      alignof(sockaddr_storage) >= alignof(sockaddr_in),
                  "sockaddr_storage alignment insufficient");

    alignas(sockaddr_storage) std::array<std::uint8_t, sizeof(sockaddr_storage)> addr{};
    socklen_t size = addr.size();
    getter(reinterpret_cast<sockaddr*>(addr.data()), size);

    if (size == sizeof(sockaddr_in)) {
        // I believe one could use std::start_lifetime_as here, but the standard library
        // doesn't implement it yet
        sockaddr_in ipv4Addr;
        std::memcpy(&ipv4Addr, addr.data(), sizeof(sockaddr_in));
        return useIpv4(ipv4Addr);
    } else if (size == sizeof(sockaddr_in6)) {
        sockaddr_in6 ipv6Addr;
        std::memcpy(&ipv6Addr, addr.data(), sizeof(sockaddr_in6));
        return useIpv6(ipv6Addr);
    }

    log_e("withSockAddr: invalid sockaddr size %u", size);
    return {};
}

Client::Client()
    : ClientBase<Client>() {
    manage(this);
}

Client::Client(int socket)
    : ClientBase<Client>(socket) {
    manage(this);
}

Client::~Client() noexcept {
    unmanage(this);
}

IPAddress Client::remoteIP() const {
    if (!isOpen()) {
        return {};
    }

    return withSockAddr(
        [&](sockaddr* addr, socklen_t size) { getpeername(_socket, addr, &size); },
        [](sockaddr_in& s) { return IPAddress(s.sin_addr.s_addr); },
        [](sockaddr_in6& s) {
            if (s.sin6_scope_id > 0xFF) {
                log_w("Client::remoteIP: IPv6 scope_id %u truncated to 8 bits",
                      s.sin6_scope_id);
            }
            return IPAddress(IPType::IPv6, s.sin6_addr.s6_addr,
                             static_cast<std::uint8_t>(s.sin6_scope_id));
        });
}

std::uint16_t Client::remotePort() const {
    if (!isOpen()) {
        return 0;
    }

    return withSockAddr(
        [&](sockaddr* addr, socklen_t size) { getpeername(_socket, addr, &size); },
        [](sockaddr_in& s) { return ntohs(s.sin_port); },
        [](sockaddr_in6& s) { return ntohs(s.sin6_port); });
}

IPAddress Client::localIP() const {
    if (!isOpen()) {
        return {};
    }

    return withSockAddr(
        [&](sockaddr* addr, socklen_t size) { getsockname(_socket, addr, &size); },
        [](sockaddr_in& s) { return IPAddress(s.sin_addr.s_addr); },
        [](sockaddr_in6& s) {
            if (s.sin6_scope_id > 0xFF) {
                log_w("Client::localIP: IPv6 scope_id %u truncated to 8 bits",
                      s.sin6_scope_id);
            }
            return IPAddress(IPType::IPv6, s.sin6_addr.s6_addr,
                             static_cast<std::uint8_t>(s.sin6_scope_id));
        });
}

std::uint16_t Client::localPort() const {
    if (!isOpen()) {
        return 0;
    }

    return withSockAddr(
        [&](sockaddr* addr, socklen_t size) { getsockname(_socket, addr, &size); },
        [](sockaddr_in& s) { return ntohs(s.sin_port); },
        [](sockaddr_in6& s) { return ntohs(s.sin6_port); });
}

void Client::onConnect(Callbacks::ConnectHandler cb, void* arg) {
    _callbacks.connectHandler = cb;
    _callbacks.connectArg = arg;
}

void Client::onDisconnect(Callbacks::DisconnectHandler cb, void* arg) {
    _callbacks.disconnectHandler = cb;
    _callbacks.disconnectArg = arg;
}

void Client::onPoll(Callbacks::PollHandler cb, void* arg) {
    _callbacks.pollHandler = cb;
    _callbacks.pollArg = arg;
}

void Client::onAck(Callbacks::SentHandler cb, void* arg) {
    _callbacks.sentHandler = cb;
    _callbacks.sentArg = arg;
}

void Client::onData(Callbacks::RecvHandler cb, void* arg) {
    _callbacks.recvHandler = cb;
    _callbacks.recvArg = arg;
}

void Client::onError(Callbacks::ErrorHandler cb, void* arg) {
    _callbacks.errorHandler = cb;
    _callbacks.errorArg = arg;
}

void Client::onTimeout(Callbacks::TimeoutHandler cb, void* arg) {
    _callbacks.timeoutHandler = cb;
    _callbacks.timeoutArg = arg;
}

std::size_t Client::ack(std::size_t len) {
    // compatibility
    return len;
}

void Client::ackLater() {
    // compatibility
}