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

  public:
    SocketConnection();
    virtual ~SocketConnection();

    inline bool isOpen() const {
        return _socket != -1;
    }

    inline std::chrono::steady_clock::time_point getLastActive() const {
        return _sock_lastactivity;
    }

    inline void setLastActive(
        std::chrono::steady_clock::time_point when = std::chrono::steady_clock::now()) {
        _sock_lastactivity = std::move(when);
    }

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
    // Will a read from this socket result in one more client?
    virtual bool _isServer() = 0;
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