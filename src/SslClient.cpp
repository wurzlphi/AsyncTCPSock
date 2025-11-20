#include "SslClient.hpp"

using namespace AsyncTcpSock;

bool SslClient::connect(IPAddress ip, std::uint16_t port) {
    return ClientBase::connect(std::move(ip), port);
}

bool SslClient::connect(const char* host, std::uint16_t port) {
#if ASYNC_TCP_SSL_ENABLED
    _hostname = host;
#endif
    return ClientBase::connect(host, port);
}

void SslClient::setRootCa(const char* rootca, const size_t len) {
#if ASYNC_TCP_SSL_ENABLED
    _root_ca = (char*)rootca;
    _root_ca_len = len;
#endif
}

void SslClient::setClientCert(const char* cli_cert, const size_t len) {
#if ASYNC_TCP_SSL_ENABLED
    _cli_cert = (char*)cli_cert;
    _cli_cert_len = len;
#endif
}

void SslClient::setClientKey(const char* cli_key, const size_t len) {
#if ASYNC_TCP_SSL_ENABLED
    _cli_key = (char*)cli_key;
    _cli_key_len = len;
#endif
}

void SslClient::setPsk(const char* psk_ident, const char* psk) {
#if ASYNC_TCP_SSL_ENABLED
    _psk_ident = psk_ident;
    _psk = psk;
#endif
}

void SslClient::_close() {
    ClientBase::_close();

#if ASYNC_TCP_SSL_ENABLED
    if (_sslctx != NULL) {
        delete _sslctx;
        _sslctx = NULL;
    }
#endif
}

bool SslClient::_processWriteQueue(std::unique_lock<std::mutex>&) {
#if ASYNC_TCP_SSL_ENABLED
    if (_sslctx != NULL) {
        r = _sslctx->write(p, n);
        if (ASYNCTCP_TLS_CAN_RETRY(r)) {
            r = -1;
            errno = EAGAIN;
        } else if (ASYNCTCP_TLS_EOF(r)) {
            r = -1;
            errno = EPIPE;
        } else if (r < 0) {
            if (errno == 0)
                errno = EIO;
        }
    } else {
        r = lwip_write(_socket, p, n);
    }
#endif
}

bool SslClient::_sockIsWriteable() {
#if ASYNC_TCP_SSL_ENABLED
    if ((_conn_state == 2 || _conn_state == 3) && _secure) {
        int res = 0;

        if (_sslctx == NULL) {
            String remIP_str = remoteIP().toString();
            const char* host_or_ip =
                _hostname.isEmpty() ? remIP_str.c_str() : _hostname.c_str();

            _sslctx = new AsyncTCP_TLS_Context();
            if (_root_ca != NULL) {
                res = _sslctx->startSSLClient(
                    _socket, host_or_ip, (const unsigned char*)_root_ca, _root_ca_len,
                    (const unsigned char*)_cli_cert, _cli_cert_len,
                    (const unsigned char*)_cli_key, _cli_key_len);
            } else if (_psk_ident != NULL) {
                res = _sslctx->startSSLClient(_socket, host_or_ip, _psk_ident, _psk);
            } else {
                res = _sslctx->startSSLClientInsecure(_socket, host_or_ip);
            }

            if (res != 0) {
                // SSL setup for AsyncTCP does not inform SSL errors
                log_e("TLS setup failed with error %d, closing socket...", res);
                _close();
                // _sslctx should be NULL after this
            }
        }

        // _handshake_done is set to FALSE on connect() if encrypted
        // connection
        if (_sslctx != NULL && res == 0)
            res = _runSSLHandshakeLoop();

        if (!_handshake_done)
            return ASYNCTCP_TLS_CAN_RETRY(res);

        // Fallthrough to ordinary successful connection
    }
#endif
}

void SslClient::_sockIsReadable() {
#if ASYNC_TCP_SSL_ENABLED
    if (_sslctx != NULL) {
        if (!_handshake_done) {
            // Handshake process has stopped for want of data, must be
            // continued here for connection to complete.
            _runSSLHandshakeLoop();

            // If handshake was successful, this will be recognized when the socket
            // next becomes writable. No other read operation should be done here.
            return;
        } else {
            r = _sslctx->read(_readBuffer, MAX_PAYLOAD_SIZE);
            if (ASYNCTCP_TLS_CAN_RETRY(r)) {
                r = -1;
                errno = EAGAIN;
            } else if (ASYNCTCP_TLS_EOF(r)) {
                // Simulate "successful" end-of-stream condition
                r = 0;
            } else if (r < 0) {
                if (errno == 0)
                    errno = EIO;
            }
        }
    } else {
        r = lwip_read(_socket, _readBuffer, MAX_PAYLOAD_SIZE);
    }
#endif
}