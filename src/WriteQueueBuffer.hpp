#ifndef ASYNCTCPSOCK_WRITEQUEUEBUFFER_HPP
#define ASYNCTCPSOCK_WRITEQUEUEBUFFER_HPP

#include <chrono>
#include <cstdint>
#include <thread>
#include <variant>
#include <vector>

#include <esp32-hal-log.h>
#include <lwip/sockets.h>

#include "Configuration.hpp"


namespace AsyncTcpSock {

struct WriteStats {
    std::size_t length;
    std::chrono::duration<std::uint32_t, std::milli> delay;
};

struct CommonWriteQueueBuffer {
    std::size_t amountWritten = 0;
    std::chrono::steady_clock::time_point queuedAt{};
    std::chrono::steady_clock::time_point writtenAt{};
    int errorCode = 0;
};

struct BorrowedWriteQueueBuffer : public CommonWriteQueueBuffer {
    std::span<const std::uint8_t> data{};
};

struct OwnedWriteQueueBuffer : public CommonWriteQueueBuffer {
    std::vector<std::uint8_t> data{};
};

using WriteQueueBuffer = std::variant<BorrowedWriteQueueBuffer, OwnedWriteQueueBuffer>;

namespace WriteQueueBufferUtil {

inline bool hasError(const WriteQueueBuffer& buf) {
    return std::visit([](auto&& it) { return it.errorCode != 0; }, buf);
}

template <class Buffer>
bool isFullyWritten_(Buffer&& buf) {
    return buf.amountWritten >= buf.data.size();
}

inline bool isFullyWritten(const WriteQueueBuffer& buf) {
    return std::visit([](auto&& it) { return isFullyWritten_(it); }, buf);
}

inline std::size_t write(WriteQueueBuffer& buf, int socket) {
    return std::visit(
        [&](auto&& it) {
            std::size_t writtenTotal = 0;

            do {
                const std::uint8_t* const start = it.data.data() + it.amountWritten;
                const std::size_t toWrite = it.data.size() - it.amountWritten;

                errno = 0;
                const ssize_t result = lwip_write(socket, start, toWrite);

                if (result >= 0) {
                    log_d_("socket %d lwip_write() wrote %d bytes", socket, result);

                    // Written some data into the socket
                    it.amountWritten += result;
                    writtenTotal += result;

                    if (isFullyWritten_(it)) {
                        // We're done
                        it.writtenAt = std::chrono::steady_clock::now();
                        it.data = {};
                        break;
                    }
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Socket is full, could not write anything
                    log_w("socket %d is full", socket);
                    break;
                } else {
                    // A write error happened that should be reported
                    it.errorCode = errno;
                    log_e("socket %d lwip_write() failed errno=%d", socket, it.errorCode);
                    break;
                }
            } while (!isFullyWritten_(it));

            return writtenTotal;
        },
        buf);
}

inline const CommonWriteQueueBuffer& asCommonView(const WriteQueueBuffer& buf) {
    return std::visit([](const auto& it) -> const CommonWriteQueueBuffer& { return it; },
                      buf);
}

}  // namespace WriteQueueBufferUtil

}  // namespace AsyncTcpSock

#endif