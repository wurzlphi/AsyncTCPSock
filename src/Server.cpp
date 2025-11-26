#include "Server.hpp"

#include "Callbacks.hpp"


using namespace AsyncTcpSock;

Server::Server(std::uint16_t port)
    : SocketConnection(true), _port(port) {
    manage(this);
    log_d_("Server created on port %d", port);
}

Server::Server(IPAddress addr, std::uint16_t port)
    : SocketConnection(true), _addr(addr), _port(port) {
    manage(this);
    log_d_("Server created on %s:%d", addr.toString().c_str(), port);
}

Server::~Server() noexcept {
    unmanage(this);
    end();
}

void Server::begin() {
    if (isOpen())
        return;

    errno = 0;
    int socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket < 0) {
        log_e("socket() error: %d (%s)", errno, strerror(errno));
        return;
    }

    sockaddr server = std::bit_cast<sockaddr>(
        sockaddr_in{.sin_len = 0,
                    .sin_family = AF_INET,
                    .sin_port = htons(_port),
                    .sin_addr = {.s_addr = static_cast<std::uint32_t>(_addr)},
                    .sin_zero = {}});

    int res = ::bind(socket, &server, sizeof(server));
    if (res < 0) {
        log_e("bind() error: %d (%s)", errno, strerror(errno));
        ::close(socket);
        return;
    }

    res = ::listen(socket, BACKLOG);
    if (res < 0) {
        log_e("listen() error: %d (%s)", errno, strerror(errno));
        ::close(socket);
        return;
    }

    _configureSocket(socket);

    log_d_("Server acquired socket %d, listening on %s:%d", socket,
          _addr.toString().c_str(), _port);
}

void Server::end() {
    if (!isOpen()) {
        return;
    }

    ::close(_socket.exchange(-1));

    log_d_("Server socket closed");
}

void Server::onClient(Callbacks::AcceptHandler cb, void* arg) {
    _callbacks.acceptHandler = cb;
    _callbacks.acceptArg = arg;
}

void Server::setNoDelay(bool noDelay) {
    _noDelay = noDelay;
}

bool Server::_sockIsWriteable() {
    // TODO Refactor to avoid empty implementations
    return false;
}

void Server::_sockIsReadable() {
    if (!_callbacks.acceptHandler) {
        return;
    }

    sockaddr_in clientInfo{};
    socklen_t clientSize = sizeof(clientInfo);
    errno = 0;
    int acceptedSocket =
        ::accept(_socket, reinterpret_cast<sockaddr*>(&clientInfo), &clientSize);

    if (acceptedSocket < 0) {
        log_e("accept() error: %d (%s)", errno, strerror(errno));
        return;
    }

    // Raw allocation... Not nice but required for API compatibility
    ClientType* client = new ClientType(acceptedSocket);
    if (!client) {
        log_e("Failed to allocate Client object for new connection");
        ::close(acceptedSocket);
        return;
    }

    client->setNoDelay(_noDelay);
    _callbacks.invoke<ServerCallbackType::ACCEPT>(client);
}

void Server::_sockDelayedConnect() {
    // Dummy impl
}

void Server::_sockPoll() {
    // Dummy impl
}

bool Server::_pendingWrite() {
    // Dummy impl
    return false;
}

void Server::_processingDone() {
    // Dummy impl
}