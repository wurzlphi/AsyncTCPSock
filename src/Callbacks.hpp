#ifndef ASYNCTCPSOCK_CALLBACKS_HPP
#define ASYNCTCPSOCK_CALLBACKS_HPP

#include <cstdint>
#include <functional>
#include <utility>

#include <esp32-hal-log.h>

#include "Configuration.hpp"

namespace AsyncTcpSock {

enum class ClientCallbackType : std::uint8_t {
    CONNECT,
    DISCONNECT,
    POLL,
    SENT,
    RECV,
    ERROR,
    TIMEOUT,
};

template <
    class Client,
    class ConnectArg = void*,
    class ConnectHandler_ = std::function<void(ConnectArg arg, Client* client)>,
    class DisconnectArg = void*,
    class DisconnectHandler_ = ConnectHandler_,
    class PollArg = void*,
    class PollHandler_ = ConnectHandler_,
    class SentArg = void*,
    class SentHandler_ = std::function<void(
        SentArg arg, Client* client, std::size_t len, std::uint32_t delayMillis)>,
    class RecvArg = void*,
    class RecvHandler_ =
        std::function<void(RecvArg arg, Client* client, void* data, std::size_t len)>,
    class ErrorArg = void*,
    class ErrorHandler_ =
        std::function<void(ErrorArg arg, Client* client, int errorCode)>,
    class TimeoutArg = void*,
    class TimeoutHandler_ =
        std::function<void(TimeoutArg arg, Client* client, std::uint32_t delayMillis)>>
struct ClientCallbacks {
    using ConnectHandler = ConnectHandler_;
    using DisconnectHandler = DisconnectHandler_;
    using PollHandler = PollHandler_;
    using SentHandler = SentHandler_;
    using RecvHandler = RecvHandler_;
    using ErrorHandler = ErrorHandler_;
    using TimeoutHandler = TimeoutHandler_;

    Client* client;

    ConnectArg connectArg{};
    ConnectHandler connectHandler{};

    DisconnectArg disconnectArg{};
    DisconnectHandler disconnectHandler{};

    PollArg pollArg{};
    PollHandler pollHandler{};

    SentArg sentArg{};
    SentHandler sentHandler{};

    RecvArg recvArg{};
    RecvHandler recvHandler{};

    ErrorArg errorArg{};
    ErrorHandler errorHandler{};

    TimeoutArg timeoutArg{};
    TimeoutHandler timeoutHandler{};

    ClientCallbacks(Client* c)
        : client(c) {
    }

    template <ClientCallbackType TYPE, class... Args>
    void invoke(Args&&... args) {
        log_d_("Invoking callback of type %d, client=%p", std::to_underlying(TYPE),
               client);

        if (!client) {
            log_e("Client is null");
            return;
        }

        if constexpr (TYPE == ClientCallbackType::CONNECT) {
            if (!connectHandler)
                return;

            std::invoke(connectHandler, connectArg, client, std::forward<Args>(args)...);
        } else if constexpr (TYPE == ClientCallbackType::DISCONNECT) {
            if (!disconnectHandler)
                return;

            std::invoke(disconnectHandler, disconnectArg, client,
                        std::forward<Args>(args)...);
        } else if constexpr (TYPE == ClientCallbackType::POLL) {
            if (!pollHandler)
                return;

            std::invoke(pollHandler, pollArg, client, std::forward<Args>(args)...);
        } else if constexpr (TYPE == ClientCallbackType::SENT) {
            if (!sentHandler)
                return;

            std::invoke(sentHandler, sentArg, client, std::forward<Args>(args)...);
        } else if constexpr (TYPE == ClientCallbackType::RECV) {
            if (!recvHandler)
                return;

            std::invoke(recvHandler, recvArg, client, std::forward<Args>(args)...);
        } else if constexpr (TYPE == ClientCallbackType::ERROR) {
            if (!errorHandler)
                return;

            std::invoke(errorHandler, errorArg, client, std::forward<Args>(args)...);
        } else if constexpr (TYPE == ClientCallbackType::TIMEOUT) {
            if (!timeoutHandler)
                return;

            std::invoke(timeoutHandler, timeoutArg, client, std::forward<Args>(args)...);

        } else {
            static_assert(false, "Invalid ClientCallbackType");
            std::unreachable();
        }
    }
};

enum class ServerCallbackType : std::uint8_t {
    ACCEPT,
};

template <class Server,
          class Client = Server::ClientType,
          class AcceptArg = void*,
          class AcceptHandler_ = std::function<void(AcceptArg arg, Client* client)>>
struct ServerCallbacks {
    using AcceptHandler = AcceptHandler_;

    Server* server;

    AcceptArg acceptArg;
    AcceptHandler acceptHandler;

    ServerCallbacks(Server* s)
        : server(s) {
    }

    template <ServerCallbackType TYPE, class... Args>
    void invoke(Args&&... args) {
        log_d_("Invoking server callback of type %d, server=%p", std::to_underlying(TYPE),
               server);

        if (!server) {
            log_e("Server is null");
            return;
        }

        if constexpr (TYPE == ServerCallbackType::ACCEPT) {
            if (!acceptHandler)
                return;

            std::invoke(acceptHandler, acceptArg, std::forward<Args>(args)...);
        } else {
            static_assert(false, "Invalid ServerCallbackType");
            std::unreachable();
        }
    }
};

}  // namespace AsyncTcpSock

#endif