#include "Client.hpp"

using namespace AsyncTcpSock;

Client::Client()
    : ClientBase<Client>() {
}

Client::Client(int socket)
    : ClientBase<Client>(socket) {
}
