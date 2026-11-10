#pragma once

#include "IrProtocolEncoder.hpp"
#include "UdpSocket.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string_view>

namespace remote {

/// Describes a single IR burst put on the (simulated) air.
struct TransmissionEvent {
    ir::Command command = ir::Command::Power;
    bool isRepeat = false;
    ir::NecFrame frame{};
    std::uint32_t binaryWord = 0;
    std::size_t pulseCount = 0;
    std::uint64_t airtimeUs = 0;
    std::size_t bytesOnWire = 0;
};

/// Simulated handheld remote: turns button presses into NEC pulse trains and
/// transmits them to a virtual device over UDP. Holding a button is modelled
/// with NEC repeat codes sent at the standard 108 ms frame period.
class RemoteController {
public:
    using TransmitListener = std::function<void(const TransmissionEvent&)>;

    RemoteController(std::uint8_t deviceAddress, net::Endpoint target, double jitterRatio = 0.0,
                     std::optional<std::uint32_t> seed = std::nullopt);

    /// Presses a button and, optionally, keeps it held for `holdRepeats` repeat codes.
    /// Returns the number of datagrams transmitted.
    std::size_t press(ir::Command command, std::size_t holdRepeats = 0);

    void setTransmitListener(TransmitListener listener) { listener_ = std::move(listener); }
    void setDeviceAddress(std::uint8_t address) noexcept { encoder_ = ir::IrProtocolEncoder{address}; }
    void setJitter(double ratio) noexcept;

    [[nodiscard]] std::uint8_t deviceAddress() const noexcept { return encoder_.deviceAddress(); }
    [[nodiscard]] double jitter() const noexcept { return jitterRatio_; }

    /// Maps user friendly key names ("power", "vol+", "ch-", "5", ...) to commands.
    [[nodiscard]] static std::optional<ir::Command> parseKey(std::string_view key);

private:
    std::size_t transmit(ir::PulseTrain train, TransmissionEvent event);
    void applyJitter(ir::PulseTrain& train);

    ir::IrProtocolEncoder encoder_;
    net::Endpoint target_;
    net::UdpSocket socket_;
    double jitterRatio_ = 0.0;
    std::mt19937 rng_;
    TransmitListener listener_;
};

} // namespace remote
