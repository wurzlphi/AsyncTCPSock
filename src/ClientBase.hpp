#ifndef ASYNCTCPSOCK_CLIENTBASE_HPP
#define ASYNCTCPSOCK_CLIENTBASE_HPP

#include <array>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <IPAddress.h>
#include <lwip/err.h>

#include "Callbacks.hpp"
#include "Configuration.hpp"
#include "SocketConnection.hpp"
#include "WriteQueueBuffer.hpp"

namespace AsyncTcpSock {

enum class ClientApiFlag : std::uint8_t {
    COPY = 0b0000'0001,  // will allocate new buffer to hold the data while sending (else
                         // will hold reference to the data given)
    MORE = 0b0000'0010   // will not send PSH flag, meaning that there should be more data
                         // to be sent before the application should react.
};

// compatibility
#define ASYNC_WRITE_FLAG_COPY (AsyncTcpSock::ClientApiFlag::COPY)
#define ASYNC_WRITE_FLAG_MORE (AsyncTcpSock::ClientApiFlag::MORE)

struct ClientApiFlags {
    using underlying_type = std::underlying_type_t<ClientApiFlag>;
    underlying_type bits{0};

    inline ClientApiFlags() = default;
    inline ClientApiFlags(ClientApiFlag value)
        : bits(std::to_underlying(value)) {
    }

    inline void set(ClientApiFlag flag) {
        bits |= std::to_underlying(flag);
    }

    inline bool test(ClientApiFlag flag) const {
        return (bits & std::to_underlying(flag)) != 0;
    }
};

enum class ConnectionState : std::uint8_t {
    DISCONNECTED,
    WAITING_FOR_DNS,
    CONNECTED,
    DISCONNECTING,
};

template <class Client>
class ClientBase : public SocketConnection {
  public:
    using Callbacks = ClientCallbacks<Client>;

    static constexpr int ERR_DNS_RESOLUTION_FAILED = -55;
    static constexpr std::size_t INITIAL_WRITE_SPACE = TCP_SND_BUF;

  protected:
    Callbacks _callbacks{static_cast<Client*>(this)};

  private:
    // This buffer can be shared for all clients since reading is performed sequentially
    // by the manager task.
    static inline std::array<std::uint8_t, TCP_MSS> SHARED_READ_BUFFER{};

    ConnectionState _state = ConnectionState::DISCONNECTED;

    std::mutex _writeMutex{};
    std::size_t _writeSpaceRemaining = INITIAL_WRITE_SPACE;
    // vector is as fast as deque in my benchmarks and actually performs slightly better
    // for smaller queue sizes
    std::vector<WriteQueueBuffer> _writeQueue{};

    IPAddress _ip{};
    std::uint16_t _port{};

    std::optional<std::chrono::steady_clock::duration> _ack_timeout =
        std::chrono::milliseconds(CONFIG_ASYNC_TCP_MAX_ACK_TIME);
    std::optional<std::chrono::steady_clock::duration> _rx_timeout = std::nullopt;
    std::chrono::steady_clock::time_point _rx_last_packet{};
    bool _ack_timeout_signaled = false;

  public:
    static void dnsFoundCallback(const char* name, const ip_addr_t* ip, void* arg);

    /// Create a client in an unconnected state.
    ClientBase();
    /// Create a client from an existing connected socket, for example from ::accept() in
    /// a server.
    ClientBase(int socket);

    ClientBase(const ClientBase& other) = delete;
    ClientBase(ClientBase&& other) = delete;

    ClientBase& operator=(const ClientBase& other) = delete;
    ClientBase& operator=(ClientBase&& other) = delete;

    ~ClientBase() noexcept override;

    virtual bool connect(IPAddress ip, std::uint16_t port);
    virtual bool connect(const char* host, std::uint16_t port);

    void close(bool now = false);
    err_enum_t abort();

    bool freeable() const;
    bool connected() const;
    bool canSend() const;
    std::size_t space() const;

    /// Add the buffer to the send queue. It will be sent by the manager task as soon as
    /// possible.
    std::size_t add(std::span<const std::uint8_t> data,
                    ClientApiFlags apiFlags = ClientApiFlag::COPY);
    std::size_t add(const std::uint8_t* data,
                    std::size_t size,
                    ClientApiFlags apiFlags = ClientApiFlag::COPY);
    // compatibility
    std::size_t add(const char* str,
                    std::size_t size = 0,
                    ClientApiFlags apiFlags = ClientApiFlag::COPY);
    /// Push everything from the send queue to LWIP to immediately send it. Calling this
    /// explicitly is unnecessary, but be aware that any callbacks will run in the
    /// calling thread if you do so.
    bool send();

    /// Adds data to the send queue and immediately attempts to send it if the queue
    /// wasn't full.
    std::size_t write(const char* str,
                      std::size_t size = 0,
                      ClientApiFlags apiFlags = ClientApiFlag::COPY);
    std::size_t write(const std::uint8_t* bytes,
                      std::size_t size,
                      ClientApiFlags apiFlags = ClientApiFlag::COPY);
    // If true, disables Nagle's algorithm (TCP_NODELAY)
    void setNoDelay(bool nodelay);
    bool getNoDelay();

    void setAckTimeout(std::optional<std::chrono::steady_clock::duration> timeout);
    void setRxTimeout(std::optional<std::chrono::steady_clock::duration> timeout);

    // compatibility
    template <class Integer>
        requires std::is_integral_v<Integer>
    void setAckTimeout(Integer milliseconds) {
        if (milliseconds <= 0) {
            setAckTimeout(std::nullopt);
            return;
        }

        setAckTimeout(
            std::chrono::steady_clock::duration(std::chrono::milliseconds(milliseconds)));
    }

    // compatibility
    template <class Integer>
        requires std::is_integral_v<Integer>
    void setRxTimeout(Integer milliseconds) {
        if (milliseconds <= 0) {
            setRxTimeout(std::nullopt);
            return;
        }

        setRxTimeout(
            std::chrono::steady_clock::duration(std::chrono::milliseconds(milliseconds)));
    }

  protected:
    // Closes the socket and clears the write queue
    virtual void _close();
    // Invokes the error callback and closes the socket - does not delete
    void _error(int errorCode);

    virtual bool _processWriteQueue(std::unique_lock<std::mutex>& writeQueueLock);
    void _cleanupWriteQueue(std::unique_lock<std::mutex>& writeQueueLock);
    void _clearWriteQueue();

    bool _checkAckTimeout();
    bool _checkRxTimeout();

    // SocketConnection
    bool _pendingWrite() override;

    bool _sockIsWriteable() override;
    void _sockIsReadable() override;

    void _sockDelayedConnect() override;
    void _sockPoll() override;
    void _processingDone() override;
};

}  // namespace AsyncTcpSock

#include "ClientBase.tpp"

#endif