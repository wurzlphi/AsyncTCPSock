#include "SocketConnection.hpp"

#include <chrono>
#include <mutex>
#include <stdexcept>

#include "esp32-hal-log.h"
#include "esp32-hal.h"
#include "esp_task_wdt.h"
#include "freertos/projdefs.h"
#include "sdkconfig.h"

using namespace AsyncTcpSock;

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

SocketConnection::SocketConnection() {
    // Add this base socket to the monitored list
    SocketConnectionManager::instance().addConnection(this);
}

SocketConnection::~SocketConnection() {
    // Remove this base socket from the monitored list
    SocketConnectionManager::instance().removeConnection(this);
}

SocketConnectionManager& SocketConnectionManager::instance() {
    static SocketConnectionManager manager;
    return manager;
}

// The main work function responsible for updating each connection's state
// according to the TCP state machine.
void SocketConnectionManager::updateConnectionStates(void*) {
    auto& manager = SocketConnectionManager::instance();

    std::vector<SocketConnection*> workingCopy;
#ifdef CONFIG_LWIP_MAX_SOCKETS
    workingCopy.reserve(CONFIG_LWIP_MAX_SOCKETS);
#endif

    while (true) {
        fd_set sockSet_r;
        fd_set sockSet_w;
        int max_sock = 0;

        // Collect all of the active sockets into socket set. Half-destroyed
        // connections should have set _socket to -1 and therefore should not
        // end up in the sockList.
        FD_ZERO(&sockSet_r);
        FD_ZERO(&sockSet_w);

        manager.iterateConnections([&](SocketConnection* it) {
            const auto socket = it->_socket.load();

            if (socket != -1) {
#ifdef CONFIG_LWIP_MAX_SOCKETS
                if (!it->_isServer() || manager.hasFreeSocket()) {
#endif
                    FD_SET(socket, &sockSet_r);
                    max_sock = std::max(max_sock, socket + 1);
#ifdef CONFIG_LWIP_MAX_SOCKETS
                }
#endif
                if (it->_pendingWrite()) {
                    FD_SET(socket, &sockSet_w);
                    max_sock = std::max(max_sock, socket + 1);
                }
            }
        });

        // Wait for activity on all monitored sockets
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = POLL_INTERVAL.count() * 1000;

        int success = select(max_sock, &sockSet_r, &sockSet_w, NULL, &tv);
        // xSemaphoreTakeRecursive(_asyncsock_mutex, (TickType_t)portMAX_DELAY);
        if (success > 0) {
            {
                // Collect and notify all writable sockets. Half-destroyed connections
                // should have set _socket to -1 and therefore should not end up in
                // the sockList.
                manager.iterateConnections([&](SocketConnection* it) {
                    if (FD_ISSET(it->_socket.load(), &sockSet_w)) {
                        workingCopy.push_back(it);
                    }
                });

                for (const auto& it : workingCopy) {
                    enter_wdt();
                    if (it->_sockIsWriteable()) {
                        it->setLastActive();
                    }
                    leave_wdt();
                }

                workingCopy.clear();
            }
            {
                // Collect and notify all readable sockets. Half-destroyed connections
                // should have set _socket to -1 and therefore should not end up in
                // the sockList.
                manager.iterateConnections([&](SocketConnection* it) {
                    if (FD_ISSET(it->_socket.load(), &sockSet_r)) {
                        workingCopy.push_back(it);
                    }
                });

                for (const auto& it : workingCopy) {
                    enter_wdt();
                    it->setLastActive();
                    it->_sockIsReadable();
                    leave_wdt();
                }

                workingCopy.clear();
            }
        }

        {
            // Collect and notify all sockets waiting for DNS completion
            manager.iterateConnections([&](SocketConnection* it) {
                // Collect socket that has finished resolving DNS (with or without
                // error)
                if (it->_isdnsfinished) {
                    workingCopy.push_back(it);
                }
            });

            for (const auto& it : workingCopy) {
                enter_wdt();
                it->_isdnsfinished = false;
                it->_sockDelayedConnect();
                leave_wdt();
            }

            workingCopy.clear();
        }
        // xSemaphoreGiveRecursive(_asyncsock_mutex);

        {
            // Collect and run activity poll on all pollable sockets
            // xSemaphoreTakeRecursive(_asyncsock_mutex, (TickType_t)portMAX_DELAY);
            manager.iterateConnections([&](SocketConnection* it) {
                const auto now = std::chrono::steady_clock::now();
                if (now - it->getLastActive() >= POLL_INTERVAL) {
                    it->setLastActive(std::move(now));
                    workingCopy.push_back(it);
                }
            });

            // Run activity poll on all pollable sockets
            for (const auto& it : workingCopy) {
                enter_wdt();
                it->_sockPoll();
                leave_wdt();
            }

            workingCopy.clear();
            // xSemaphoreGiveRecursive(_asyncsock_mutex);
        }
    }
}

void SocketConnectionManager::addConnection(SocketConnection* connection) {
    if (connection != nullptr) {
        std::lock_guard lock(managerMutex);
        connections.push_back(connection);
    }
}

void SocketConnectionManager::removeConnection(SocketConnection* connection) {
    if (connection != nullptr) {
        std::lock_guard lock(managerMutex);
        std::erase(connections, connection);
    }
}

void SocketConnectionManager::iterateConnections(
    const std::function<void(SocketConnection*)>& fn) const {
    if (fn) {
        std::lock_guard lock(managerMutex);
        for (SocketConnection* connection : connections) {
            // The function must not try to acquire the manager mutex, otherwise it
            // deadlocks. This is unproblematic for every usage in
            // updateConnectionStates(...).
            fn(connection);
        }
    }
}

bool SocketConnectionManager::hasFreeSocket() const {
    return connections.size() < CONFIG_LWIP_MAX_SOCKETS;
}

SocketConnectionManager::SocketConnectionManager()
    : managerMutex(), connections(), workerThread(nullptr) {
    connections.reserve(CONFIG_LWIP_MAX_SOCKETS);

    log_i(
        "Creating worker task for AsyncTCPSock, name: %s, stack: %d, prio: %d, affinity: "
        "%d",
        TASK_NAME.data(), TASK_STACK_SIZE, TASK_PRIORITY, TASK_CORE_AFFINITY);
    auto result =
        xTaskCreateUniversal(updateConnectionStates, TASK_NAME.data(), TASK_STACK_SIZE,
                             nullptr, TASK_PRIORITY, &workerThread, TASK_CORE_AFFINITY);
    if (result != pdPASS) {
        log_e(
            "Failed to create FreeRTOS task for AsyncTCPSock. TCP communication will not "
            "be available.");
        throw std::runtime_error("Failed to create AsyncTCPSock task");
    }
}

SocketConnectionManager::~SocketConnectionManager() noexcept {
    if (workerThread != nullptr) {
        vTaskDelete(workerThread);
    }
}