#ifndef ASYNCTCPSOCK_CONFIGURATION_HPP
#define ASYNCTCPSOCK_CONFIGURATION_HPP

#define ASYNC_TCP_ENABLE_DEBUG_LOG 0
#if ASYNC_TCP_ENABLE_DEBUG_LOG
#include <esp32-hal-log.h>
#define log_d_(...) log_d(__VA_ARGS__)
#else
#define log_d_(...) \
    do {            \
    } while (0)
#endif

#ifndef CONFIG_ASYNC_TCP_RUNNING_CORE
// If core is not defined, then we are running in Arduino or PIO
#define CONFIG_ASYNC_TCP_RUNNING_CORE -1  // any available core
#define CONFIG_ASYNC_TCP_USE_WDT 1  // if enabled, adds between 33us and 200us per event
#endif

#ifndef CONFIG_ASYNC_TCP_STACK
#define CONFIG_ASYNC_TCP_STACK 16384  // 8192 * 2
#endif

#ifndef CONFIG_ASYNC_TCP_TASK_PRIORITY
#define CONFIG_ASYNC_TCP_TASK_PRIORITY 18
#endif

#ifndef CONFIG_ASYNC_TCP_POLL_INTERVAL
#define CONFIG_ASYNC_TCP_POLL_INTERVAL 125
#endif

#ifndef CONFIG_ASYNC_TCP_MAX_PAYLOAD_SIZE
#define CONFIG_ASYNC_TCP_MAX_PAYLOAD_SIZE 1360
#endif

#ifndef CONFIG_ASYNC_TCP_MAX_ACK_TIME
#define CONFIG_ASYNC_TCP_MAX_ACK_TIME 5000
#endif

#ifndef CONFIG_ASYNC_TCP_SSL_HANDSHAKE_TIMEOUT
#define CONFIG_ASYNC_TCP_SSL_HANDSHAKE_TIMEOUT 5000
#endif

#endif