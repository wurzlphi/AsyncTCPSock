#ifndef ASYNCTCPSOCK_SSLCLIENT_HPP
#define ASYNCTCPSOCK_SSLCLIENT_HPP

#include "Client.hpp"

namespace AsyncTcpSock {

class SslClient : public Client {
    bool connect(IPAddress ip, uint16_t port) override;
    bool connect(const char* host, uint16_t port) override;

    void setRootCa(const char* rootca, const size_t len);
    void setClientCert(const char* cli_cert, const size_t len);
    void setClientKey(const char* cli_key, const size_t len);
    void setPsk(const char* psk_ident, const char* psk);

  protected:
    int _runSSLHandshakeLoop();

    // ClientBase
    void _close() override;
    bool _processWriteQueue(std::unique_lock<std::mutex>& writeQueueLock) override;

    // SocketConnection
    bool _sockIsWriteable() override;
    void _sockIsReadable() override;
};

}  // namespace AsyncTcpSock

#endif