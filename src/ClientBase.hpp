#ifndef ASYNCTCPSOCK_CLIENTBASE_HPP
#define ASYNCTCPSOCK_CLIENTBASE_HPP

#include <bitset>
#include <chrono>
#include <cstdint>
#include <utility>

#include <IPAddress.h>

#include "SocketConnection.hpp"

namespace AsyncTcpSock {

enum class ClientApiFlag : std::uint8_t {
    COPY = 0b0000'0001,  // will allocate new buffer to hold the data while sending (else
                         // will hold reference to the data given)
    MORE = 0b0000'0010   // will not send PSH flag, meaning that there should be more data
                         // to be sent before the application should react.
};

using ClientApiFlags = std::bitset<8>;

enum class ConnectionState : std::uint8_t {
    DISCONNECTED,
    WAITING_FOR_DNS,
    CONNECTED,
};

class ClientBase : public SocketConnection {
    ConnectionState _state = ConnectionState::DISCONNECTED;

    IPAddress _ip{};
    std::uint16_t _port{};

    std::chrono::steady_clock::time_point _rx_last_packet{};

  public:
    static void dnsFoundCallback(const char* name, const ip_addr_t* ip, void* arg);

    bool connect(IPAddress ip, std::uint16_t port);
    bool connect(const char* host, std::uint16_t port);
    void close(bool now = false);

    int8_t abort();
    bool free();

    bool canSend() {
        return space() > 0;
    }
    size_t space();

    /// Add the buffer to the send queue
    size_t add(const char* data,
               size_t size,
               ClientApiFlags apiflags = std::to_underlying(ClientApiFlag::COPY));
    /// Push everything from the send queue to LWIP to send it
    bool send();

    /// write equals add()+send()
    size_t write(const char* data);
    size_t write(const char* data,
                 size_t size,
                 ClientApiFlags apiflags = std::to_underlying(
                     ClientApiFlag::COPY));  // only when canSend() == true
};

}  // namespace AsyncTcpSock

#endif