#ifndef ASYNCTCPSOCK_SSLCLIENT_HPP
#define ASYNCTCPSOCK_SSLCLIENT_HPP

#include "ClientBase.hpp"

namespace AsyncTcpSock {

class SslClient : public ClientBase {
    bool connect(IPAddress ip, uint16_t port);
    bool connect(const char* host, uint16_t port);

    void setRootCa(const char* rootca, const size_t len);
    void setClientCert(const char* cli_cert, const size_t len);
    void setClientKey(const char* cli_key, const size_t len);
    void setPsk(const char* psk_ident, const char* psk);

  protected:  
  // ClientBase
  void _close() override;
  bool _flushWriteQueue() override;

  // SocketConnection
  bool _sockIsWriteable() override;
  void _sockIsReadable() override;
};

}  // namespace AsyncTcpSock

#endif