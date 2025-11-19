#ifndef ASYNCTCPSOCK_CLIENTBASE_HPP
#define ASYNCTCPSOCK_CLIENTBASE_HPP

#include <array>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

#include <IPAddress.h>
#include <lwip/err.h>

#include "Callbacks.hpp"
#include "SocketConnection.hpp"
#include "WriteQueueBuffer.hpp"

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
    static constexpr int ERR_DNS_RESOLUTION_FAILED = -55;
    static constexpr std::size_t INITIAL_WRITE_SPACE = TCP_SND_BUF;

    // This buffer can be shared for all clients since reading is performed sequentially
    // by the manager task.
    static inline std::array<std::uint8_t, TCP_MSS> SHARED_READ_BUFFER{};

    Callbacks<ClientBase> _callbacks;  // TODO Replace with actual client argument

    ConnectionState _state = ConnectionState::DISCONNECTED;

    std::mutex _writeMutex{};
    std::size_t _writeSpaceRemaining = INITIAL_WRITE_SPACE;
    // vector is as fast as deque in my benchmarks and actually performs slightly better
    // for smaller queue sizes
    std::vector<WriteQueueBuffer> _writeQueue{};

    IPAddress _ip{};
    std::uint16_t _port{};

    std::optional<std::chrono::steady_clock::duration> _ack_timeout = std::nullopt;
    std::optional<std::chrono::steady_clock::duration> _rx_timeout = std::nullopt;
    std::chrono::steady_clock::time_point _rx_last_packet{};
    bool _ack_timeout_signaled = false;

  public:
    static void dnsFoundCallback(const char* name, const ip_addr_t* ip, void* arg);

    bool connect(IPAddress ip, std::uint16_t port);
    bool connect(const char* host, std::uint16_t port);

    void close(bool now = false);
    err_enum_t abort();

    bool freeable() const;
    bool connected() const;
    bool canSend() const;
    std::size_t space() const;

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

  protected:
    virtual void _close();
    virtual void _error(int errorCode);

    virtual bool _processWriteQueue(std::unique_lock<std::mutex>& writeQueueLock);
    void _cleanupWriteQueue(std::unique_lock<std::mutex>& writeQueueLock);
    void _clearWriteQueue();

    bool _checkAckTimeout();
    bool _checkRxTimeout();

    // SocketConnection
    bool _sockIsWriteable() override;
    void _sockIsReadable() override;

    void _sockDelayedConnect() override;
    void _sockPoll() override;
};

}  // namespace AsyncTcpSock

#endif