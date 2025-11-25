#ifndef ASYNCTCPSOCK_SERVER_HPP
#define ASYNCTCPSOCK_SERVER_HPP

#include "Client.hpp"
#include "SocketConnection.hpp"

namespace AsyncTcpSock {

class Server : public SocketConnection {
  public:
    using ClientType = Client;
    using Callbacks = ServerCallbacks<Server>;

    static constexpr int BACKLOG = 5;

  private:
    IPAddress _addr{IP_ADDR_ANY};
    std::uint16_t _port = 0;

    bool _noDelay = true;  // Whether new connections will use TCP_NODELAY
    Callbacks _callbacks{this};

  public:
    Server(std::uint16_t port);
    Server(IPAddress addr, std::uint16_t port);

    ~Server() noexcept override;

    Server(const Server& other) = delete;
    Server(Server&& other) = delete;

    Server& operator=(const Server& other) = delete;
    Server& operator=(Server&& other) = delete;

    void begin();
    void end();

    void onClient(Callbacks::AcceptHandler cb, void* arg = nullptr);
    // Disable Nagle's algorithm on new connections
    void setNoDelay(bool noDelay);

  protected:
    bool _sockIsWriteable() override;
    void _sockIsReadable() override;
    void _sockDelayedConnect() override;
    void _sockPoll() override;
    bool _pendingWrite() override;
    void _processingDone() override;
};

}  // namespace AsyncTcpSock

#endif