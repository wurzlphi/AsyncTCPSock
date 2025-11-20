/*
  Reimplementation of an asynchronous TCP library for Espressif MCUs, using
  BSD sockets.

  Copyright (c) 2020 Alex Villacís Lasso.

  Original AsyncTCP API Copyright (c) 2016 Hristo Gochkov. All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef ASYNCTCP_H_
#define ASYNCTCP_H_

#include <functional>
#include <deque>

#include "IPAddress.h"

#if ASYNC_TCP_SSL_ENABLED
#include <ssl_client.h>
#include "AsyncTCP_TLS_Context.h"
#endif

#include "SocketConnection.hpp"

extern "C" {
    #include "lwip/err.h"
    #include "lwip/sockets.h"
}

class AsyncClient;

typedef std::function<void(void*, AsyncClient*)> AcConnectHandler;
typedef std::function<void(void*, AsyncClient*, size_t len, uint32_t time)> AcAckHandler;
typedef std::function<void(void*, AsyncClient*, int8_t error)> AcErrorHandler;
typedef std::function<void(void*, AsyncClient*, void *data, size_t len)> AcDataHandler;
//typedef std::function<void(void*, AsyncClient*, struct pbuf *pb)> AcPacketHandler;
typedef std::function<void(void*, AsyncClient*, uint32_t time)> AcTimeoutHandler;

class AsyncClient : public AsyncTcpSock::SocketConnection
{
  public:
    AsyncClient(int sockfd = -1);
    ~AsyncClient();

    uint32_t getRemoteAddress();
    uint16_t getRemotePort();
    uint32_t getLocalAddress();
    uint16_t getLocalPort();

    //compatibility
    IPAddress remoteIP();
    uint16_t  remotePort();
    IPAddress localIP();
    uint16_t  localPort();

    void onConnect(AcConnectHandler cb, void* arg = 0);     //on successful connect
    void onDisconnect(AcConnectHandler cb, void* arg = 0);  //disconnected
    void onAck(AcAckHandler cb, void* arg = 0);             //ack received
    void onError(AcErrorHandler cb, void* arg = 0);         //unsuccessful connect or error
    void onData(AcDataHandler cb, void* arg = 0);           //data received
    void onTimeout(AcTimeoutHandler cb, void* arg = 0);     //ack timeout
    void onPoll(AcConnectHandler cb, void* arg = 0);        //every 125ms when connected

    // The following functions are just for API compatibility and do nothing
    size_t ack(size_t len)  { return len; }
    void ackLater() {}

    const char * errorToString(int8_t error);
//    const char * stateToString();

  private:

    AcConnectHandler _connect_cb;
    void* _connect_cb_arg;
    AcConnectHandler _discard_cb;
    void* _discard_cb_arg;
    AcAckHandler _sent_cb;
    void* _sent_cb_arg;
    AcErrorHandler _error_cb;
    void* _error_cb_arg;
    AcDataHandler _recv_cb;
    void* _recv_cb_arg;
    AcTimeoutHandler _timeout_cb;
    void* _timeout_cb_arg;
    AcConnectHandler _poll_cb;
    void* _poll_cb_arg;

    uint32_t _rx_last_packet;
    uint32_t _rx_since_timeout;
    uint32_t _ack_timeout;

#if ASYNC_TCP_SSL_ENABLED
    size_t _root_ca_len;
    char* _root_ca;
    size_t _cli_cert_len;
    char* _cli_cert;
    size_t _cli_key_len;
    char* _cli_key;
    bool _secure;
    bool _handshake_done;
    const char* _psk_ident;
    const char* _psk;

    String _hostname;
    AsyncTCP_TLS_Context * _sslctx;
#endif // ASYNC_TCP_SSL_ENABLED

    // The following private struct represents a buffer enqueued with the add()
    // method. Each of these buffers are flushed whenever the socket becomes
    // writable
    typedef struct {
      uint8_t * data;     // Pointer to data queued for write
      uint32_t  length;   // Length of data queued for write
      uint32_t  written;  // Length of data written to socket so far
      uint32_t  queued_at;// Timestamp at which this data buffer was queued
      uint32_t  written_at; // Timestamp at which this data buffer was completely written
      int       write_errno;  // If != 0, errno value while writing this buffer
      bool      owned;    // If true, we malloc'ed the data and should be freed after completely written.
                          // If false, app owns the memory and should ensure it remains valid until acked
    } queued_writebuf;

    // Internal struct used to implement sent buffer notification
    typedef struct {
      uint32_t length;
      uint32_t delay;
    } notify_writebuf;

    // Queue of buffers to write to socket
    std::mutex writeMutex;
    std::deque<queued_writebuf> _writeQueue;
    bool _ack_timeout_signaled = false;

    // Remaining space willing to queue for writing
    uint32_t _writeSpaceRemaining;

    void _removeAllCallbacks();
    
#if ASYNC_TCP_SSL_ENABLED
    int _runSSLHandshakeLoop(void);
#endif
};

#if ASYNC_TCP_SSL_ENABLED
typedef std::function<int(void* arg, const char *filename, uint8_t **buf)> AcSSlFileHandler;
#endif

class AsyncServer : public AsyncTcpSock::SocketConnection
{
  public:
    AsyncServer(IPAddress addr, uint16_t port);
    AsyncServer(uint16_t port);
    ~AsyncServer();
    void onClient(AcConnectHandler cb, void* arg);
#if ASYNC_TCP_SSL_ENABLED
    // Dummy, so it compiles with ESP Async WebServer library enabled.
    void onSslFileRequest(AcSSlFileHandler cb, void* arg) {};
    void beginSecure(const char *cert, const char *private_key_file, const char *password) {};
#endif
    void begin();
    void end();

    void setNoDelay(bool nodelay) { _noDelay = nodelay; }
    bool getNoDelay() { return _noDelay; }
    uint8_t status();

  protected:
    uint16_t _port;
    IPAddress _addr;

    bool _noDelay;
    AcConnectHandler _connect_cb;
    void* _connect_cb_arg;

    // Listening socket is readable on incoming connection
    void _sockIsReadable() override;
    bool _sockIsWriteable() override;
    void _sockPoll() override;
    void _sockDelayedConnect() override;

    bool _pendingWrite() override;
};


#endif /* ASYNCTCP_H_ */
