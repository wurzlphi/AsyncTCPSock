#ifndef ASYNCTCPSOCK_SOCKETCONNECTION_HPP
#define ASYNCTCPSOCK_SOCKETCONNECTION_HPP

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <variant>
#include <vector>

#include <esp32-hal-log.h>
#include <esp32-hal.h>
#include <freertos/idf_additions.h>
#include <portmacro.h>

#include "Configuration.hpp"

namespace AsyncTcpSock {

template <class Connection>
void manage(Connection* conn);
template <class Connection>
void unmanage(Connection* conn);

template <class Impl>
concept ManagedClient = requires(Impl impl) {
    requires !Impl::IS_SERVER;
    // Action to take on a writable socket
    { impl._sockIsWriteable() } -> std::same_as<bool>;
    // Action to take on a readable socket
    { impl._sockIsReadable() } -> std::same_as<void>;
    // Action to take when DNS-resolution is finished
    { impl._sockDelayedConnect() } -> std::same_as<void>;
    // Action to take for an idle socket when the polling timer runs out
    { impl._sockPoll() } -> std::same_as<void>;
    // Action to take when processing is done for this socket in the manager task. Do
    // cleanup here.
    { impl._processingDone() } -> std::same_as<void>;
    // Test if there is data pending to be written
    { impl._pendingWrite() } -> std::same_as<bool>;
};

template <class Impl>
concept ManagedServer = requires(Impl impl) {
    requires Impl::IS_SERVER;
    // Action to take on a readable socket
    { impl._sockIsReadable() } -> std::same_as<void>;
};

namespace detail {
template <class T>
struct isVariantOfClientPointers {
    static constexpr bool value = false;
};

template <class... Elements>
struct isVariantOfClientPointers<std::variant<Elements...>> {
    static constexpr bool value = (std::conjunction_v<std::is_pointer<Elements>...>) &&
                                  (ManagedClient<std::remove_pointer_t<Elements>> && ...);
};

template <class T>
struct isVariantOfServerPointers {
    static constexpr bool value = false;
};

template <class... Elements>
struct isVariantOfServerPointers<std::variant<Elements...>> {
    static constexpr bool value = (std::conjunction_v<std::is_pointer<Elements>...>) &&
                                  (ManagedServer<std::remove_pointer_t<Elements>> && ...);
};
}  // namespace detail

template <class Variant>
concept ClientVariantType = detail::isVariantOfClientPointers<Variant>::value;

template <class Variant>
concept ServerVariantType = detail::isVariantOfServerPointers<Variant>::value;

/**
 * Formerly AsyncSocketBase
 */
struct SocketConnection {
    std::atomic<int> _socket = -1;
    std::atomic<bool> _dnsFinished = false;
    std::chrono::steady_clock::time_point _lastActive = std::chrono::steady_clock::now();

    SocketConnection();
    SocketConnection(int socket);

    virtual ~SocketConnection() noexcept = default;

    SocketConnection(const SocketConnection& other) = delete;
    SocketConnection(SocketConnection&& other) = delete;

    SocketConnection& operator=(const SocketConnection& other) = delete;
    SocketConnection& operator=(SocketConnection&& other) = delete;

    bool isOpen() const;

    std::chrono::steady_clock::time_point getLastActive() const;
    void setLastActive(
        std::chrono::steady_clock::time_point when = std::chrono::steady_clock::now());

    // Action to take on a writable socket
    virtual bool _sockIsWriteable() = 0;
    // Action to take on a readable socket
    virtual void _sockIsReadable() = 0;
    // Action to take when DNS-resolution is finished
    virtual void _sockDelayedConnect() = 0;
    // Action to take for an idle socket when the polling timer runs out
    virtual void _sockPoll() = 0;
    // Action to take when processing is done for this socket in the manager task. Do
    // cleanup here.
    virtual void _processingDone() = 0;
    // Test if there is data pending to be written
    virtual bool _pendingWrite() = 0;

    void _configureSocket(int socket);
};

template <ClientVariantType ClientVariant, ServerVariantType ServerVariant>
class SocketConnectionManager {
    static constexpr std::string_view TASK_NAME = "Async TCP Sock Worker";
    static constexpr std::uint32_t TASK_STACK_SIZE = CONFIG_ASYNC_TCP_STACK;
    static constexpr UBaseType_t TASK_PRIORITY = CONFIG_ASYNC_TCP_TASK_PRIORITY;
    static constexpr BaseType_t TASK_CORE_AFFINITY = CONFIG_ASYNC_TCP_RUNNING_CORE;
    static constexpr std::chrono::milliseconds POLL_INTERVAL{
        CONFIG_ASYNC_TCP_POLL_INTERVAL};

    mutable std::mutex managerMutex;
    std::vector<ClientVariant> clients;
    std::vector<ServerVariant> servers;
    TaskHandle_t workerThread;

  public:
    static SocketConnectionManager<ClientVariant, ServerVariant>& instance();

    template <ManagedClient Client>
    void addConnection(Client* client) {
        log_d_("Adding client %p", client);

        if (client != nullptr) {
            std::lock_guard lock(managerMutex);
            clients.emplace_back(client);
        }
    }
    template <ManagedServer Server>
    void addConnection(Server* server) {
        log_d_("Adding server %p", server);

        if (server != nullptr) {
            std::lock_guard lock(managerMutex);
            servers.emplace_back(server);
        }
    }

    template <ManagedClient Client>
    void removeConnection(Client* client) {
        log_d_("Removing client %p", client);

        if (client != nullptr) {
            std::lock_guard lock(managerMutex);
            std::erase(clients, ClientVariant{client});
        }
    }
    template <ManagedServer Server>
    void removeConnection(Server* server) {
        log_d_("Removing client %p", server);

        if (server != nullptr) {
            std::lock_guard lock(managerMutex);
            std::erase(servers, ServerVariant{server});
        }
    }

  private:
    template <class Func>
    void iterateClients(Func&& fn) const {
        std::lock_guard lock(managerMutex);
        for (const ClientVariant& client : clients) {
            // The function must not try to acquire the manager mutex, otherwise it
            // deadlocks. This is unproblematic for every usage in
            // updateConnectionStates(...).
            std::visit(fn, client);
        }
    }

    template <class Func>
    void iterateServers(Func&& fn) const {
        std::lock_guard lock(managerMutex);
        for (const ServerVariant& server : servers) {
            // The function must not try to acquire the manager mutex, otherwise it
            // deadlocks. This is unproblematic for every usage in
            // updateConnectionStates(...).
            std::visit(fn, server);
        }
    }

    constexpr bool hasFreeSocket() const {
#ifdef CONFIG_LWIP_MAX_SOCKETS
        // mutex must have been locked
        return clients.size() + servers.size() < CONFIG_LWIP_MAX_SOCKETS;
#else
        return true;
#endif
    }

    static void updateConnectionStates(void*);

    SocketConnectionManager();
    ~SocketConnectionManager() noexcept;
};

}  // namespace AsyncTcpSock

#include "SocketConnection.tpp"

#endif