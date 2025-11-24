#ifndef ASYNCTCPSOCK_CLIENT_HPP
#define ASYNCTCPSOCK_CLIENT_HPP

#include "ClientBase.hpp"

namespace AsyncTcpSock {

constexpr const char* errorToString(int8_t error) {
    switch (error) {
        case ERR_OK:
            return "OK";
        case ERR_MEM:
            return "Out of memory error";
        case ERR_BUF:
            return "Buffer error";
        case ERR_TIMEOUT:
            return "Timeout";
        case ERR_RTE:
            return "Routing problem";
        case ERR_INPROGRESS:
            return "Operation in progress";
        case ERR_VAL:
            return "Illegal value";
        case ERR_WOULDBLOCK:
            return "Operation would block";
        case ERR_USE:
            return "Address in use";
        case ERR_ALREADY:
            return "Already connected";
        case ERR_CONN:
            return "Not connected";
        case ERR_IF:
            return "Low-level netif error";
        case ERR_ABRT:
            return "Connection aborted";
        case ERR_RST:
            return "Connection reset";
        case ERR_CLSD:
            return "Connection closed";
        case ERR_ARG:
            return "Illegal argument";
        case -55:
            return "DNS failed";
        default:
            return "UNKNOWN";
    }
}

class Client : public ClientBase<Client> {
  public:
    static constexpr const char* errorToString(int8_t error) {
        return AsyncTcpSock::errorToString(error);
    }

    Client();
    Client(int socket);

    ~Client() noexcept override;

    Client(const Client& other) = delete;
    Client(Client&& other) = delete;

    Client& operator=(const Client& other) = delete;
    Client& operator=(Client&& other) = delete;

    IPAddress remoteIP() const;
    std::uint16_t remotePort() const;
    IPAddress localIP() const;
    std::uint16_t localPort() const;

    // on successful connect
    void onConnect(Callbacks::ConnectHandler cb, void* arg = nullptr);
    // disconnected
    void onDisconnect(Callbacks::DisconnectHandler cb, void* arg = nullptr);
    // every 125ms when connected
    void onPoll(Callbacks::PollHandler cb, void* arg = nullptr);
    // ack received
    void onAck(Callbacks::SentHandler cb, void* arg = nullptr);
    // data received
    void onData(Callbacks::RecvHandler cb, void* arg = nullptr);
    // unsuccessful connect or error
    void onError(Callbacks::ErrorHandler cb, void* arg = nullptr);
    // ack timeout
    void onTimeout(Callbacks::TimeoutHandler cb, void* arg = nullptr);

    // The following functions are just for API compatibility and do nothing
    std::size_t ack(std::size_t len);
    void ackLater();
};

}  // namespace AsyncTcpSock

#endif