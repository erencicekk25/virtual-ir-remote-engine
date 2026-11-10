#include "UdpSocket.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <stdexcept>
#include <system_error>
#include <utility>

namespace net {

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
using AddressLength = int;
constexpr NativeSocket kInvalidNative = INVALID_SOCKET;

int lastError() noexcept { return WSAGetLastError(); }
/// WSAECONNRESET is reported on UDP sockets after an ICMP "port unreachable".
bool isTransient(int error) noexcept {
    return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK || error == WSAECONNRESET;
}
void closeNative(NativeSocket socket) noexcept { ::closesocket(socket); }

/// Owns the process-wide Winsock initialisation for as long as the program runs.
class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};
        const int rc = ::WSAStartup(MAKEWORD(2, 2), &data);
        if (rc != 0) {
            throw std::system_error(rc, std::system_category(), "WSAStartup failed");
        }
    }
    ~WinsockRuntime() { ::WSACleanup(); }

    WinsockRuntime(const WinsockRuntime&) = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;
};

void ensureNetworkRuntime() {
    static const WinsockRuntime runtime;
}
#else
using NativeSocket = int;
using AddressLength = socklen_t;
constexpr NativeSocket kInvalidNative = -1;

int lastError() noexcept { return errno; }
bool isTransient(int error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR || error == ECONNREFUSED;
}
void closeNative(NativeSocket socket) noexcept { ::close(socket); }
void ensureNetworkRuntime() {}
#endif

[[noreturn]] void throwLastError(const char* what) {
    throw std::system_error(lastError(), std::system_category(), what);
}

sockaddr_in makeAddress(const std::string& host, std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        throw std::invalid_argument("invalid IPv4 address: " + host);
    }
    return address;
}

} // namespace

UdpSocket::UdpSocket() {
    ensureNetworkRuntime();
    const NativeSocket socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == kInvalidNative) {
        throwLastError("failed to create UDP socket");
    }
    handle_ = static_cast<Handle>(socket);
}

UdpSocket::~UdpSocket() {
    close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : handle_(std::exchange(other.handle_, kInvalidHandle)) {}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, kInvalidHandle);
    }
    return *this;
}

void UdpSocket::close() noexcept {
    if (handle_ != kInvalidHandle) {
        closeNative(static_cast<NativeSocket>(handle_));
        handle_ = kInvalidHandle;
    }
}

void UdpSocket::bind(const std::string& host, std::uint16_t port) {
    const sockaddr_in address = makeAddress(host, port);
    if (::bind(static_cast<NativeSocket>(handle_), reinterpret_cast<const sockaddr*>(&address),
               static_cast<AddressLength>(sizeof(address))) != 0) {
        throwLastError("failed to bind UDP socket");
    }
}

std::uint16_t UdpSocket::localPort() const {
    sockaddr_in address{};
    AddressLength length = static_cast<AddressLength>(sizeof(address));
    if (::getsockname(static_cast<NativeSocket>(handle_), reinterpret_cast<sockaddr*>(&address),
                      &length) != 0) {
        throwLastError("failed to query local socket address");
    }
    return ntohs(address.sin_port);
}

bool UdpSocket::waitReadable() const {
    const auto socket = static_cast<NativeSocket>(handle_);
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socket, &readSet);

    timeval timeout{};
    timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(receiveTimeout_.count() / 1000);
    timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>((receiveTimeout_.count() % 1000) * 1000);

#ifdef _WIN32
    const int ready = ::select(0, &readSet, nullptr, nullptr, &timeout);
#else
    int ready = 0;
    do {
        ready = ::select(socket + 1, &readSet, nullptr, nullptr, &timeout);
    } while (ready < 0 && errno == EINTR);
#endif
    if (ready < 0) {
        throwLastError("failed to wait for UDP socket");
    }
    return ready > 0;
}

std::size_t UdpSocket::sendTo(const std::vector<std::uint8_t>& payload, const Endpoint& target) {
    const sockaddr_in address = makeAddress(target.host, target.port);
#ifdef _WIN32
    const int sent = ::sendto(static_cast<NativeSocket>(handle_),
                              reinterpret_cast<const char*>(payload.data()),
                              static_cast<int>(payload.size()), 0,
                              reinterpret_cast<const sockaddr*>(&address),
                              static_cast<int>(sizeof(address)));
#else
    const ssize_t sent = ::sendto(handle_, payload.data(), payload.size(), 0,
                                  reinterpret_cast<const sockaddr*>(&address), sizeof(address));
#endif
    if (sent < 0) {
        throwLastError("failed to send UDP datagram");
    }
    return static_cast<std::size_t>(sent);
}

std::optional<std::vector<std::uint8_t>> UdpSocket::receive(std::size_t maxBytes) {
    if (!waitReadable()) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> buffer(maxBytes);
    sockaddr_in sender{};
    AddressLength senderLength = static_cast<AddressLength>(sizeof(sender));

#ifdef _WIN32
    const int received = ::recvfrom(static_cast<NativeSocket>(handle_),
                                    reinterpret_cast<char*>(buffer.data()),
                                    static_cast<int>(buffer.size()), 0,
                                    reinterpret_cast<sockaddr*>(&sender), &senderLength);
#else
    const ssize_t received = ::recvfrom(handle_, buffer.data(), buffer.size(), 0,
                                        reinterpret_cast<sockaddr*>(&sender), &senderLength);
#endif

    if (received < 0) {
        if (isTransient(lastError())) {
            return std::nullopt;
        }
        throwLastError("failed to receive UDP datagram");
    }

    buffer.resize(static_cast<std::size_t>(received));
    return buffer;
}

} // namespace net
