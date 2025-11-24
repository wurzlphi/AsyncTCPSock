#include "Server.hpp"

using namespace AsyncTcpSock;

Server::Server(std::uint16_t port)
    : SocketConnection(true), _port(port) {
    manage();
}

Server::Server(IPAddress addr, std::uint16_t port)
    : SocketConnection(true), _addr(addr), _port(port) {
    manage();
}

Server::~Server() noexcept {
    unmanage();
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

    sockaddr_in server{.sin_len = 0,
                       .sin_family = AF_INET,
                       .sin_port = htons(_port),
                       .sin_addr = {.s_addr = static_cast<std::uint32_t>(_addr)},
                       .sin_zero = {}};

    int res = ::bind(socket, reinterpret_cast<sockaddr*>(&server), sizeof(server));
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

    res = ::fcntl(socket, F_SETFL, O_NONBLOCK);
    if (res < 0) {
        log_w("fcntl() error: %d (%s)", errno, strerror(errno));
    }

    _socket = socket;
}

void Server::end() {
    if (!isOpen()) {
        return;
    }

    ::close(_socket.exchange(-1));
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
    _callbacks.acceptHandler(_callbacks.acceptArg, client);
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