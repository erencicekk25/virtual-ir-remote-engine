#pragma once

#include "IrProtocolEncoder.hpp"
#include "UdpSocket.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace tv {

enum class PowerState { Standby, On };
enum class InputSource { Tv, Hdmi1, Hdmi2, Av };

[[nodiscard]] std::string_view toString(PowerState state) noexcept;
[[nodiscard]] std::string_view toString(InputSource source) noexcept;

struct TvState {
    static constexpr int kMinVolume = 0;
    static constexpr int kMaxVolume = 100;
    static constexpr int kMinChannel = 1;
    static constexpr int kMaxChannel = 999;
    static constexpr std::size_t kMaxChannelDigits = 3;

    PowerState power = PowerState::Standby;
    int volume = 20;
    bool muted = false;
    int channel = 1;
    InputSource input = InputSource::Tv;
    std::string pendingDigits;
};

[[nodiscard]] std::string describe(const TvState& state);

// ---------------------------------------------------------------------------
// Signal parser
// ---------------------------------------------------------------------------

enum class DecodeStatus {
    Frame,          ///< A full, integrity-checked NEC frame.
    Repeat,         ///< A NEC repeat code (button still held).
    TimingError,    ///< Pulse timings do not match the NEC specification.
    IntegrityError, ///< Timings were fine but the inverted bytes did not match.
};

[[nodiscard]] std::string_view toString(DecodeStatus status) noexcept;

struct DecodeResult {
    DecodeStatus status = DecodeStatus::TimingError;
    ir::NecFrame frame{};
    std::uint32_t rawWord = 0;
    std::string detail;
};

/// Recovers NEC frames from raw mark/space timings with a configurable
/// tolerance, the same way a real IR receiver's firmware would.
class IrSignalParser {
public:
    static constexpr double kDefaultTolerance = 0.25;

    explicit IrSignalParser(double tolerance = kDefaultTolerance) noexcept;

    [[nodiscard]] DecodeResult parse(const ir::PulseTrain& train) const;
    [[nodiscard]] double tolerance() const noexcept { return tolerance_; }

private:
    [[nodiscard]] bool matches(std::uint32_t actualUs, std::uint32_t expectedUs) const noexcept;

    double tolerance_;
};

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

/// Pure TV behaviour: applies decoded commands to the device state.
class TvStateMachine {
public:
    /// Applies a command and returns a human-readable description of the transition.
    std::string apply(ir::Command command);

    /// Re-applies the last repeatable command (NEC repeat code).
    std::string applyRepeat();

    [[nodiscard]] const TvState& state() const noexcept { return state_; }

private:
    std::string handleStandby(ir::Command command);
    std::string handleOn(ir::Command command);
    std::string commitPendingChannel();

    TvState state_;
    std::optional<ir::Command> lastRepeatable_;
};

// ---------------------------------------------------------------------------
// Network-facing device
// ---------------------------------------------------------------------------

/// Virtual TV that listens for serialized IR pulse trains on a UDP port,
/// decodes them and drives its state machine on a dedicated worker thread.
/// The worker is started explicitly and is always stopped and joined in the
/// destructor (RAII).
class VirtualTvDevice {
public:
    using EventListener = std::function<void(const std::string&)>;

    static constexpr std::chrono::milliseconds kRepeatWindow{200};
    static constexpr std::chrono::milliseconds kPollInterval{100};

    VirtualTvDevice(std::uint8_t deviceAddress, std::uint16_t port = 0,
                    EventListener listener = {}, double tolerance = IrSignalParser::kDefaultTolerance);
    ~VirtualTvDevice();

    VirtualTvDevice(const VirtualTvDevice&) = delete;
    VirtualTvDevice& operator=(const VirtualTvDevice&) = delete;
    VirtualTvDevice(VirtualTvDevice&&) = delete;
    VirtualTvDevice& operator=(VirtualTvDevice&&) = delete;

    void start();
    void stop() noexcept;

    [[nodiscard]] net::Endpoint endpoint() const;
    [[nodiscard]] std::uint8_t deviceAddress() const noexcept { return address_; }
    [[nodiscard]] TvState snapshot() const;
    [[nodiscard]] std::uint64_t processedPackets() const;

    /// Waits until at least `count` packets have been handled in total.
    bool waitForProcessed(std::uint64_t count, std::chrono::milliseconds timeout) const;

private:
    using Clock = std::chrono::steady_clock;

    void run() noexcept;
    void handlePacket(const std::vector<std::uint8_t>& packet);
    std::string handleDecoded(const DecodeResult& result);
    void emit(const std::string& message) const;

    const std::uint8_t address_;
    net::UdpSocket socket_;
    std::uint16_t port_ = 0;
    IrSignalParser parser_;
    EventListener listener_;

    mutable std::mutex mutex_;
    mutable std::condition_variable processedCv_;
    TvStateMachine machine_;
    std::uint64_t processed_ = 0;
    std::optional<Clock::time_point> lastAcceptedAt_;

    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace tv
