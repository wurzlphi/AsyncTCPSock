#ifndef ASYNCTCPSOCK_ASYNCTCPSOCKETCONNECTION_HPP
#define ASYNCTCPSOCK_ASYNCTCPSOCKETCONNECTION_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>
#include <vector>

#include <freertos/idf_additions.h>
#include <portmacro.h>

#include "Configuration.hpp"

namespace AsyncTcpSock {

/**
 * Formerly AsyncSocketBase
 */
struct SocketConnection {
    friend class SocketConnectionManager;

  protected:
    std::atomic<int> _socket = -1;
    std::atomic<bool> _isdnsfinished = false;
    std::chrono::steady_clock::time_point _sock_lastactivity =
        std::chrono::steady_clock::now();

  private:
    bool _isServer = false;

  public:
    SocketConnection(bool isServer);
    SocketConnection(int socket, bool isServer);

    virtual ~SocketConnection() = default;

    SocketConnection(const SocketConnection& other) = delete;
    SocketConnection(SocketConnection&& other) = delete;

    SocketConnection& operator=(const SocketConnection& other) = delete;
    SocketConnection& operator=(SocketConnection&& other) = delete;

    bool isOpen() const;

    std::chrono::steady_clock::time_point getLastActive() const;
    void setLastActive(
        std::chrono::steady_clock::time_point when = std::chrono::steady_clock::now());

    // If true, disables Nagle's algorithm (TCP_NODELAY)
    void setNoDelay(bool nodelay);
    bool getNoDelay();

  protected:
    // Action to take on a writable socket
    virtual bool _sockIsWriteable() = 0;
    // Action to take on a readable socket
    virtual void _sockIsReadable() = 0;
    // Action to take when DNS-resolution is finished
    virtual void _sockDelayedConnect() = 0;
    // Action to take for an idle socket when the polling timer runs out
    virtual void _sockPoll() = 0;

    // Test if there is data pending to be written
    virtual bool _pendingWrite() = 0;

    void _setSocket(int socket);

    // Must be called manually in constructor/destructor of derived classes to ensure
    // correct behavior.
    // TODO Maybe refactor into a wrapper class
    void manage();
    void unmanage();
};

class SocketConnectionManager {
    static constexpr std::string_view TASK_NAME = "Async TCP Sock Worker";
    static constexpr std::uint32_t TASK_STACK_SIZE = CONFIG_ASYNC_TCP_STACK;
    static constexpr UBaseType_t TASK_PRIORITY = CONFIG_ASYNC_TCP_TASK_PRIORITY;
    static constexpr BaseType_t TASK_CORE_AFFINITY = CONFIG_ASYNC_TCP_RUNNING_CORE;
    static constexpr std::chrono::milliseconds POLL_INTERVAL{
        CONFIG_ASYNC_TCP_POLL_INTERVAL};

    mutable std::mutex managerMutex;
    std::vector<SocketConnection*> connections;
    TaskHandle_t workerThread;

  public:
    static SocketConnectionManager& instance();

    void addConnection(SocketConnection* connection);
    void removeConnection(SocketConnection* connection);
    void iterateConnections(const std::function<void(SocketConnection*)>& fn) const;
    bool hasFreeSocket() const;

  private:
    static void updateConnectionStates(void*);

    SocketConnectionManager();
    ~SocketConnectionManager() noexcept;
};

}  // namespace AsyncTcpSock

#endif