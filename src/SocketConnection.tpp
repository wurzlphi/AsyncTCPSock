#ifndef ASYNCTCPSOCK_SOCKETCONNECTION_TPP
#define ASYNCTCPSOCK_SOCKETCONNECTION_TPP

#include "SocketConnection.hpp"

//

#include <chrono>
#include <mutex>
#include <stdexcept>

#include <esp32-hal-log.h>
#include <esp32-hal.h>
#include <esp_task_wdt.h>
#include <lwip/sockets.h>
#include <sdkconfig.h>

namespace AsyncTcpSock {

static void enter_wdt() {
#if CONFIG_ASYNC_TCP_USE_WDT
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        log_e("Failed to add async task to WDT");
    }
#endif
}

static void leave_wdt() {
#if CONFIG_ASYNC_TCP_USE_WDT
    if (esp_task_wdt_delete(NULL) != ESP_OK) {
        log_e("Failed to remove loop task from WDT");
    }
#endif
}

//
// SocketConnection
//
inline SocketConnection::SocketConnection() {
}

inline SocketConnection::SocketConnection(int socket)
    : SocketConnection() {
    _configureSocket(socket);
}

inline bool SocketConnection::isOpen() const {
    return _socket != -1;
}

inline std::chrono::steady_clock::time_point SocketConnection::getLastActive() const {
    return _lastActive;
}

inline void SocketConnection::setLastActive(std::chrono::steady_clock::time_point when) {
    _lastActive = when;
}

inline void SocketConnection::_configureSocket(int socket) {
    int res = fcntl(socket, F_SETFL, fcntl(socket, F_GETFL, 0) | O_NONBLOCK);
    if (res < 0) {
        log_e("fcntl() error: %d (%s)", errno, strerror(errno));
        ::close(socket);
    }

    _socket = socket;
}

//
// SocketConnectionManager
//
template <ClientVariantType ClientVariant, ServerVariantType ServerVariant>
SocketConnectionManager<ClientVariant, ServerVariant>&
SocketConnectionManager<ClientVariant, ServerVariant>::instance() {
    static SocketConnectionManager<ClientVariant, ServerVariant> manager;
    return manager;
}

// The main work function responsible for updating each connection's state
// according to the TCP state machine.
template <ClientVariantType ClientVariant, ServerVariantType ServerVariant>
void SocketConnectionManager<ClientVariant, ServerVariant>::updateConnectionStates(
    void*) {
    auto& manager = SocketConnectionManager::instance();

    std::vector<ClientVariant> clientsProcessing;
    std::vector<ServerVariant> serversProcessing;

    log_d_("AsyncTCPSock worker task started");

    while (true) {
        fd_set sockSet_r;
        fd_set sockSet_w;
        int max_sock = 0;

        // Collect all of the active sockets into socket set. Half-destroyed
        // connections should have set _socket to -1 and therefore should not
        // end up in the sockList.
        FD_ZERO(&sockSet_r);
        FD_ZERO(&sockSet_w);

        log_d_("Collecting active sockets for select()...");

        manager.iterateClients([&](auto&& it) {
            const int socket = it->_socket.load();

            if (socket != -1) {
                FD_SET(socket, &sockSet_r);
                max_sock = std::max(max_sock, socket + 1);

                if (it->_pendingWrite()) {
                    FD_SET(socket, &sockSet_w);
                    max_sock = std::max(max_sock, socket + 1);
                }
            }
        });
        manager.iterateServers([&](auto&& it) {
            const int socket = it->_socket.load();

            if (socket != -1) {
                if (manager.hasFreeSocket()) {
                    FD_SET(socket, &sockSet_r);
                    max_sock = std::max(max_sock, socket + 1);
                }
            }
        });

        // Wait for activity on all monitored sockets
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = POLL_INTERVAL.count() * 1000;

        int success = select(max_sock, &sockSet_r, &sockSet_w, NULL, &tv);
        if (success > 0) {
            {
                log_d_("Writing to writable client sockets...");

                // Collect and notify all writable sockets. Half-destroyed connections
                // should have set _socket to -1 and therefore should not end up in
                // the sockList.
                manager.iterateClients([&](auto&& it) {
                    if (FD_ISSET(it->_socket.load(), &sockSet_w)) {
                        clientsProcessing.push_back(it);
                    }
                });

                for (const ClientVariant& client : clientsProcessing) {
                    enter_wdt();
                    std::visit(
                        [](auto&& c) {
                            const bool activity = c->_sockIsWriteable();
                            if (activity) {
                                c->setLastActive();
                            }
                        },
                        client);
                    leave_wdt();
                }

                clientsProcessing.clear();
            }
            {
                log_d_("Reading from readable client sockets...");

                // Collect and notify all readable sockets. Half-destroyed connections
                // should have set _socket to -1 and therefore should not end up in
                // the sockList.
                manager.iterateClients([&](auto&& it) {
                    if (FD_ISSET(it->_socket.load(), &sockSet_r)) {
                        clientsProcessing.push_back(it);
                    }
                });

                for (const ClientVariant& client : clientsProcessing) {
                    enter_wdt();
                    std::visit(
                        [](auto&& c) {
                            c->setLastActive();
                            c->_sockIsReadable();
                        },
                        client);
                    leave_wdt();
                }

                clientsProcessing.clear();
            }
            {
                log_d_("Reading from readable server sockets...");

                // Collect and notify all readable sockets. Half-destroyed connections
                // should have set _socket to -1 and therefore should not end up in
                // the sockList.
                manager.iterateServers([&](auto&& it) {
                    if (FD_ISSET(it->_socket.load(), &sockSet_r)) {
                        serversProcessing.push_back(it);
                    }
                });

                for (const ServerVariant& server : serversProcessing) {
                    enter_wdt();
                    std::visit(
                        [](auto&& server) {
                            server->setLastActive();
                            server->_sockIsReadable();
                        },
                        server);
                    leave_wdt();
                }

                serversProcessing.clear();
            }
        }

        {
            log_d_("Updating clients with finished DNS resolution...");

            // Collect and notify all sockets waiting for DNS completion
            manager.iterateClients([&](auto&& it) {
                // Collect socket that has finished resolving DNS (with or without
                // error)
                if (it->_dnsFinished) {
                    clientsProcessing.push_back(it);
                }
            });

            for (const ClientVariant& client : clientsProcessing) {
                enter_wdt();
                std::visit(
                    [](auto&& c) {
                        c->_dnsFinished = false;
                        c->_sockDelayedConnect();
                    },
                    client);
                leave_wdt();
            }

            clientsProcessing.clear();
        }

        {
            log_d_("Polling clients to check timeouts...");

            // Collect and run activity poll on all pollable sockets
            manager.iterateClients([&](auto&& it) {
                const auto now = std::chrono::steady_clock::now();

                if (now - it->getLastActive() >= POLL_INTERVAL) {
                    it->setLastActive(std::move(now));
                    clientsProcessing.push_back(it);
                }
            });

            // Run activity poll on all pollable sockets
            for (const ClientVariant& client : clientsProcessing) {
                enter_wdt();
                std::visit([](auto&& c) { c->_sockPoll(); }, client);
                leave_wdt();
            }

            clientsProcessing.clear();
        }

        {
            log_d_("Cleaning up clients...");

            // Collect and run activity poll on all pollable sockets
            manager.iterateClients([&](auto&& it) { clientsProcessing.push_back(it); });

            // Run activity poll on all pollable sockets
            for (const ClientVariant& client : clientsProcessing) {
                std::visit([](auto&& c) { c->_processingDone(); }, client);
            }

            clientsProcessing.clear();
        }
    }
}

template <ClientVariantType ClientVariant, ServerVariantType ServerVariant>
SocketConnectionManager<ClientVariant, ServerVariant>::SocketConnectionManager()
    : managerMutex(), clients(), servers(), workerThread(nullptr) {
    clients.reserve(CONFIG_LWIP_MAX_SOCKETS);
    servers.reserve(2);

    log_i(
        "Creating worker task for AsyncTCPSock, name: %s, stack: %d, prio: %d, "
        "affinity: "
        "%d",
        TASK_NAME.data(), TASK_STACK_SIZE, TASK_PRIORITY, TASK_CORE_AFFINITY);
    auto result =
        xTaskCreateUniversal(updateConnectionStates, TASK_NAME.data(), TASK_STACK_SIZE,
                             nullptr, TASK_PRIORITY, &workerThread, TASK_CORE_AFFINITY);
    if (result != pdPASS) {
        log_e(
            "Failed to create FreeRTOS task for AsyncTCPSock. TCP communication will "
            "not "
            "be available.");
        throw std::runtime_error("Failed to create AsyncTCPSock task");
    }
}

template <ClientVariantType ClientVariant, ServerVariantType ServerVariant>
SocketConnectionManager<ClientVariant,
                        ServerVariant>::~SocketConnectionManager() noexcept {
    if (workerThread != nullptr) {
        vTaskDelete(workerThread);
    }
}

}  // namespace AsyncTcpSock

#endif