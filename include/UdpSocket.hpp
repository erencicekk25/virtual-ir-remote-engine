#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace net {

struct Endpoint {
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
};

/// Move-only RAII wrapper around a native IPv4 UDP socket.
/// The underlying handle is always released in the destructor, and the
/// platform network runtime (Winsock on Windows) is initialised on demand.
class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    /// Binds to the given IPv4 address. Port 0 lets the OS pick a free port.
    void bind(const std::string& host, std::uint16_t port);

    [[nodiscard]] std::uint16_t localPort() const;

    /// Maximum time receive() waits for a datagram. Implemented with select()
    /// rather than SO_RCVTIMEO, which can leave Windows sockets in an
    /// indeterminate state after a timeout.
    void setReceiveTimeout(std::chrono::milliseconds timeout) noexcept { receiveTimeout_ = timeout; }

    /// Returns the number of bytes sent.
    std::size_t sendTo(const std::vector<std::uint8_t>& payload, const Endpoint& target);

    /// Blocks until a datagram arrives or the receive timeout expires.
    /// Returns std::nullopt on timeout.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> receive(std::size_t maxBytes);

    [[nodiscard]] bool isValid() const noexcept { return handle_ != kInvalidHandle; }

private:
    using Handle = std::intptr_t;
    static constexpr Handle kInvalidHandle = -1;

    void close() noexcept;
    [[nodiscard]] bool waitReadable() const;

    Handle handle_ = kInvalidHandle;
    std::chrono::milliseconds receiveTimeout_{1000};
};

} // namespace net
