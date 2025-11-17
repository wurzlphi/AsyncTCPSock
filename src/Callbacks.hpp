#ifndef ASYNCTCPSOCK_CALLBACKS_HPP
#define ASYNCTCPSOCK_CALLBACKS_HPP

#include <cstdint>
#include <functional>
#include <utility>

namespace AsyncTcpSock {

enum class CallbackType : std::uint8_t {
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
        std::function<void(TimeoutArg arg, Client* client, std::uint32_t timestamp)>>
struct Callbacks {
    using ConnectHandler = ConnectHandler_;
    using DisconnectHandler = DisconnectHandler_;
    using PollHandler = PollHandler_;
    using SentHandler = SentHandler_;
    using RecvHandler = RecvHandler_;
    using ErrorHandler = ErrorHandler_;
    using TimeoutHandler = TimeoutHandler_;

    Client* client;

    ConnectArg connectArg;
    ConnectHandler connectHandler;

    DisconnectArg disconnectArg;
    DisconnectHandler disconnectHandler;

    PollArg pollArg;
    PollHandler pollHandler;

    SentArg sentArg;
    SentHandler sentHandler;

    RecvArg recvArg;
    RecvHandler recvHandler;

    ErrorArg errorArg;
    ErrorHandler errorHandler;

    TimeoutArg timeoutArg;
    TimeoutHandler timeoutHandler;

    template <class... Args>
    void invoke(CallbackType type, Args&&... args) const {
        switch (type) {
            using enum CallbackType;
            case CONNECT:
                return std::invoke(connectHandler, connectArg, client,
                                   std::forward<Args>(args)...);
            case DISCONNECT:
                return std::invoke(disconnectHandler, disconnectArg, client,
                                   std::forward<Args>(args)...);
            case POLL:
                return std::invoke(pollHandler, pollArg, client,
                                   std::forward<Args>(args)...);
            case SENT:
                return std::invoke(sentHandler, sentArg, client,
                                   std::forward<Args>(args)...);
            case RECV:
                return std::invoke(recvHandler, recvArg, client,
                                   std::forward<Args>(args)...);
            case ERROR:
                return std::invoke(errorHandler, errorArg, client,
                                   std::forward<Args>(args)...);
            case TIMEOUT:
                return std::invoke(timeoutHandler, timeoutArg, client,
                                   std::forward<Args>(args)...);
            default:
                std::unreachable();
        }
    }
};

}  // namespace AsyncTcpSock

#endif