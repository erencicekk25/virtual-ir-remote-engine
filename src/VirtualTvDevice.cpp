#include "VirtualTvDevice.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <sstream>

namespace tv {

namespace {

std::string hexByte(std::uint8_t value) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "0x%02X", static_cast<unsigned>(value));
    return buffer;
}

InputSource nextInput(InputSource source) noexcept {
    switch (source) {
    case InputSource::Tv:    return InputSource::Hdmi1;
    case InputSource::Hdmi1: return InputSource::Hdmi2;
    case InputSource::Hdmi2: return InputSource::Av;
    case InputSource::Av:    return InputSource::Tv;
    }
    return InputSource::Tv;
}

int wrapChannel(int channel) noexcept {
    if (channel > TvState::kMaxChannel) {
        return TvState::kMinChannel;
    }
    if (channel < TvState::kMinChannel) {
        return TvState::kMaxChannel;
    }
    return channel;
}

} // namespace

std::string_view toString(PowerState state) noexcept {
    return state == PowerState::On ? "ON" : "STANDBY";
}

std::string_view toString(InputSource source) noexcept {
    switch (source) {
    case InputSource::Tv:    return "TV";
    case InputSource::Hdmi1: return "HDMI1";
    case InputSource::Hdmi2: return "HDMI2";
    case InputSource::Av:    return "AV";
    }
    return "?";
}

std::string_view toString(DecodeStatus status) noexcept {
    switch (status) {
    case DecodeStatus::Frame:          return "Frame";
    case DecodeStatus::Repeat:         return "Repeat";
    case DecodeStatus::TimingError:    return "TimingError";
    case DecodeStatus::IntegrityError: return "IntegrityError";
    }
    return "?";
}

std::string describe(const TvState& state) {
    std::ostringstream out;
    out << "power=" << toString(state.power);
    if (state.power == PowerState::On) {
        out << " volume=" << state.volume << (state.muted ? " (muted)" : "")
            << " channel=" << state.channel
            << " input=" << toString(state.input);
        if (!state.pendingDigits.empty()) {
            out << " typing=" << state.pendingDigits << '_';
        }
    }
    return out.str();
}

// ---------------------------------------------------------------------------
// IrSignalParser
// ---------------------------------------------------------------------------

IrSignalParser::IrSignalParser(double tolerance) noexcept
    : tolerance_(std::clamp(tolerance, 0.05, 0.40)) {}

bool IrSignalParser::matches(std::uint32_t actualUs, std::uint32_t expectedUs) const noexcept {
    const double expected = static_cast<double>(expectedUs);
    const double delta = static_cast<double>(actualUs) - expected;
    return (delta < 0 ? -delta : delta) <= expected * tolerance_;
}

DecodeResult IrSignalParser::parse(const ir::PulseTrain& train) const {
    using namespace ir::nec;

    DecodeResult result;
    const auto& d = train.durationsUs;

    const auto fail = [&result](DecodeStatus status, std::string detail) {
        result.status = status;
        result.detail = std::move(detail);
        return result;
    };

    const double carrierDelta =
        static_cast<double>(train.carrierHz) - static_cast<double>(kCarrierFrequencyHz);
    if ((carrierDelta < 0 ? -carrierDelta : carrierDelta) > kCarrierFrequencyHz * 0.10) {
        return fail(DecodeStatus::TimingError,
                    "carrier " + std::to_string(train.carrierHz) + " Hz is outside the 38 kHz band");
    }

    if (d.size() < kRepeatPulseCount || d.size() % 2 == 0) {
        return fail(DecodeStatus::TimingError,
                    "pulse train must contain an odd number of pulses (got " +
                        std::to_string(d.size()) + ")");
    }
    if (!matches(d[0], kLeaderMarkUs)) {
        return fail(DecodeStatus::TimingError,
                    "leader mark " + std::to_string(d[0]) + " us is not ~9000 us");
    }

    // Repeat code: 9 ms mark, 2.25 ms space, 562 us mark.
    if (d.size() == kRepeatPulseCount) {
        if (!matches(d[1], kRepeatSpaceUs) || !matches(d[2], kTrailingMarkUs)) {
            return fail(DecodeStatus::TimingError, "malformed repeat code");
        }
        result.status = DecodeStatus::Repeat;
        return result;
    }

    if (d.size() != kFramePulseCount) {
        return fail(DecodeStatus::TimingError,
                    "expected " + std::to_string(kFramePulseCount) + " pulses, got " +
                        std::to_string(d.size()));
    }
    if (!matches(d[1], kLeaderSpaceUs)) {
        return fail(DecodeStatus::TimingError,
                    "leader space " + std::to_string(d[1]) + " us is not ~4500 us");
    }

    std::uint32_t word = 0;
    for (std::size_t bit = 0; bit < kBitsPerFrame; ++bit) {
        const std::uint32_t mark = d[2 + bit * 2];
        const std::uint32_t space = d[3 + bit * 2];

        if (!matches(mark, kBitMarkUs)) {
            return fail(DecodeStatus::TimingError,
                        "bit " + std::to_string(bit) + ": mark " + std::to_string(mark) +
                            " us is out of tolerance");
        }
        if (matches(space, kOneSpaceUs)) {
            word |= (1U << bit);
        } else if (!matches(space, kZeroSpaceUs)) {
            return fail(DecodeStatus::TimingError,
                        "bit " + std::to_string(bit) + ": space " + std::to_string(space) +
                            " us is neither a 0 nor a 1");
        }
    }

    if (!matches(d.back(), kTrailingMarkUs)) {
        return fail(DecodeStatus::TimingError, "missing trailing mark");
    }

    result.rawWord = word;
    const auto frame = ir::unpackFrame(word);
    if (!frame) {
        return fail(DecodeStatus::IntegrityError, "inverted address/command bytes do not match");
    }

    result.status = DecodeStatus::Frame;
    result.frame = *frame;
    return result;
}

// ---------------------------------------------------------------------------
// TvStateMachine
// ---------------------------------------------------------------------------

std::string TvStateMachine::apply(ir::Command command) {
    lastRepeatable_ = ir::isRepeatable(command) ? std::optional<ir::Command>{command} : std::nullopt;
    return state_.power == PowerState::Standby ? handleStandby(command) : handleOn(command);
}

std::string TvStateMachine::applyRepeat() {
    if (!lastRepeatable_) {
        return "repeat ignored: last command is not repeatable";
    }
    if (state_.power == PowerState::Standby) {
        return "repeat ignored: device is in standby";
    }
    return "(held) " + handleOn(*lastRepeatable_);
}

std::string TvStateMachine::handleStandby(ir::Command command) {
    if (command == ir::Command::Power) {
        state_.power = PowerState::On;
        return "STANDBY -> ON, resuming channel " + std::to_string(state_.channel);
    }
    lastRepeatable_.reset();
    return "ignored: device is in standby";
}

std::string TvStateMachine::handleOn(ir::Command command) {
    using ir::Command;

    if (const auto digit = ir::digitValue(command)) {
        state_.pendingDigits.push_back(static_cast<char>('0' + *digit));
        if (state_.pendingDigits.size() >= TvState::kMaxChannelDigits) {
            return commitPendingChannel();
        }
        return "channel entry: " + state_.pendingDigits + "_";
    }

    switch (command) {
    case Command::Power:
        state_.power = PowerState::Standby;
        state_.pendingDigits.clear();
        lastRepeatable_.reset();
        return "ON -> STANDBY";

    case Command::VolumeUp:
        state_.muted = false;
        state_.volume = std::min(state_.volume + 1, TvState::kMaxVolume);
        return "volume " + std::to_string(state_.volume);

    case Command::VolumeDown:
        state_.muted = false;
        state_.volume = std::max(state_.volume - 1, TvState::kMinVolume);
        return "volume " + std::to_string(state_.volume);

    case Command::Mute:
        state_.muted = !state_.muted;
        return state_.muted ? "muted" : "unmuted (volume " + std::to_string(state_.volume) + ")";

    case Command::ChannelUp:
        state_.pendingDigits.clear();
        state_.channel = wrapChannel(state_.channel + 1);
        return "channel " + std::to_string(state_.channel);

    case Command::ChannelDown:
        state_.pendingDigits.clear();
        state_.channel = wrapChannel(state_.channel - 1);
        return "channel " + std::to_string(state_.channel);

    case Command::Input:
        state_.input = nextInput(state_.input);
        return "input " + std::string{toString(state_.input)};

    case Command::Ok:
        if (state_.pendingDigits.empty()) {
            return "OK pressed (nothing to confirm)";
        }
        return commitPendingChannel();

    default:
        return std::string{ir::toString(command)} + " has no action";
    }
}

std::string TvStateMachine::commitPendingChannel() {
    const int requested = std::stoi(state_.pendingDigits);
    state_.pendingDigits.clear();
    if (requested < TvState::kMinChannel || requested > TvState::kMaxChannel) {
        return "channel " + std::to_string(requested) + " does not exist";
    }
    state_.channel = requested;
    return "channel " + std::to_string(state_.channel);
}

// ---------------------------------------------------------------------------
// VirtualTvDevice
// ---------------------------------------------------------------------------

VirtualTvDevice::VirtualTvDevice(std::uint8_t deviceAddress, std::uint16_t port,
                                 EventListener listener, double tolerance)
    : address_(deviceAddress), parser_(tolerance), listener_(std::move(listener)) {
    socket_.bind("127.0.0.1", port);
    socket_.setReceiveTimeout(kPollInterval);
    port_ = socket_.localPort();
}

VirtualTvDevice::~VirtualTvDevice() {
    stop();
}

void VirtualTvDevice::start() {
    if (running_.exchange(true)) {
        return;
    }
    worker_ = std::thread(&VirtualTvDevice::run, this);
}

void VirtualTvDevice::stop() noexcept {
    running_.store(false);
    if (worker_.joinable()) {
        worker_.join();
    }
}

net::Endpoint VirtualTvDevice::endpoint() const {
    return net::Endpoint{"127.0.0.1", port_};
}

TvState VirtualTvDevice::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return machine_.state();
}

std::uint64_t VirtualTvDevice::processedPackets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return processed_;
}

bool VirtualTvDevice::waitForProcessed(std::uint64_t count, std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return processedCv_.wait_for(lock, timeout, [&] { return processed_ >= count; });
}

void VirtualTvDevice::run() noexcept {
    while (running_.load()) {
        try {
            const auto packet = socket_.receive(ir::wire::kMaxPacketSize + 1);
            if (packet) {
                handlePacket(*packet);
            }
        } catch (const std::exception& error) {
            emit(std::string{"receiver error: "} + error.what());
        } catch (...) {
            emit("receiver error: unknown exception");
        }
    }
}

void VirtualTvDevice::handlePacket(const std::vector<std::uint8_t>& packet) {
    std::string message;
    const auto train = ir::wire::deserialize(packet);
    if (!train) {
        message = "rejected " + std::to_string(packet.size()) + "-byte packet: invalid wire format";
    } else {
        message = handleDecoded(parser_.parse(*train));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++processed_;
    }
    emit(message);
    processedCv_.notify_all();
}

std::string VirtualTvDevice::handleDecoded(const DecodeResult& result) {
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);

    switch (result.status) {
    case DecodeStatus::TimingError:
    case DecodeStatus::IntegrityError:
        lastAcceptedAt_.reset();
        return "decode failed (" + std::string{toString(result.status)} + "): " + result.detail;

    case DecodeStatus::Repeat: {
        if (!lastAcceptedAt_ || now - *lastAcceptedAt_ > kRepeatWindow) {
            return "stray repeat code ignored (no recent frame)";
        }
        lastAcceptedAt_ = now;
        const std::string transition = machine_.applyRepeat();
        return transition + "  | " + describe(machine_.state());
    }

    case DecodeStatus::Frame: {
        if (result.frame.address != address_) {
            lastAcceptedAt_.reset();
            return "frame for address " + hexByte(result.frame.address) + " ignored (mine is " +
                   hexByte(address_) + ")";
        }
        const auto command = ir::commandFromCode(result.frame.command);
        if (!command) {
            lastAcceptedAt_.reset();
            return "unknown command code " + hexByte(result.frame.command);
        }
        lastAcceptedAt_ = now;
        const std::string transition = machine_.apply(*command);
        return std::string{ir::toString(*command)} + ": " + transition + "  | " +
               describe(machine_.state());
    }
    }
    return "unhandled decode status";
}

void VirtualTvDevice::emit(const std::string& message) const {
    if (listener_) {
        try {
            listener_(message);
        } catch (...) {
            // A misbehaving listener must never take down the receiver thread.
        }
    }
}

} // namespace tv
