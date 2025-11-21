#ifndef ASYNCTCPSOCK_SERVER_HPP
#define ASYNCTCPSOCK_SERVER_HPP

#include "Client.hpp"
#include "SocketConnection.hpp"

namespace AsyncTcpSock {

class Server : public SocketConnection {
  public:
    using ClientType = Client;
    using Callbacks = ServerCallbacks<Server>;

  private:
    IPAddress _addr{IP_ADDR_ANY};
    std::uint16_t _port = 0;

    bool _noDelay = true;  // Whether new connections will use TCP_NODELAY
    Callbacks _callbacks;

  public:
    Server(std::uint16_t port);
    Server(IPAddress addr, std::uint16_t port);

    Server(const Server& other) = delete;
    Server(Server&& other) = delete;

    Server& operator=(const Server& other) = delete;
    Server& operator=(Server&& other) = delete;

    ~Server() noexcept override;

    void begin();
    void end();

    void onClient(Callbacks::AcceptHandler cb, void* arg = nullptr);
    void setNoDelay(bool noDelay);
};

}  // namespace AsyncTcpSock

#endif