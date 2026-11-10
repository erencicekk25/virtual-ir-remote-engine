#include "IrProtocolEncoder.hpp"

#include <array>
#include <numeric>
#include <stdexcept>

namespace ir {

namespace {

struct CommandInfo {
    Command command;
    std::string_view name;
    bool repeatable;
};

constexpr std::array<CommandInfo, 18> kCommandTable{{
    {Command::ChannelUp,   "ChannelUp",   true},
    {Command::ChannelDown, "ChannelDown", true},
    {Command::VolumeUp,    "VolumeUp",    true},
    {Command::VolumeDown,  "VolumeDown",  true},
    {Command::Power,       "Power",       false},
    {Command::Mute,        "Mute",        false},
    {Command::Input,       "Input",       false},
    {Command::Digit0,      "Digit0",      false},
    {Command::Digit1,      "Digit1",      false},
    {Command::Digit2,      "Digit2",      false},
    {Command::Digit3,      "Digit3",      false},
    {Command::Digit4,      "Digit4",      false},
    {Command::Digit5,      "Digit5",      false},
    {Command::Digit6,      "Digit6",      false},
    {Command::Digit7,      "Digit7",      false},
    {Command::Digit8,      "Digit8",      false},
    {Command::Digit9,      "Digit9",      false},
    {Command::Ok,          "Ok",          false},
}};

const CommandInfo* findInfo(Command command) noexcept {
    for (const auto& info : kCommandTable) {
        if (info.command == command) {
            return &info;
        }
    }
    return nullptr;
}

constexpr std::array<std::uint8_t, 4> kMagic{'I', 'R', 'P', 'T'};
constexpr std::uint8_t kWireVersion = 1;

void appendU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

std::uint16_t readU16(const std::vector<std::uint8_t>& in, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>((in[offset] << 8) | in[offset + 1]);
}

std::uint32_t readU32(const std::vector<std::uint8_t>& in, std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(in[offset]) << 24) |
           (static_cast<std::uint32_t>(in[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(in[offset + 2]) << 8) |
           static_cast<std::uint32_t>(in[offset + 3]);
}

std::uint8_t inverted(std::uint8_t value) noexcept {
    return static_cast<std::uint8_t>(~value);
}

} // namespace

// ---------------------------------------------------------------------------
// Command helpers
// ---------------------------------------------------------------------------

std::string_view toString(Command command) noexcept {
    const auto* info = findInfo(command);
    return info != nullptr ? info->name : std::string_view{"Unknown"};
}

std::optional<Command> commandFromCode(std::uint8_t code) noexcept {
    for (const auto& info : kCommandTable) {
        if (static_cast<std::uint8_t>(info.command) == code) {
            return info.command;
        }
    }
    return std::nullopt;
}

bool isRepeatable(Command command) noexcept {
    const auto* info = findInfo(command);
    return info != nullptr && info->repeatable;
}

std::optional<int> digitValue(Command command) noexcept {
    const auto code = static_cast<std::uint8_t>(command);
    const auto first = static_cast<std::uint8_t>(Command::Digit0);
    const auto last = static_cast<std::uint8_t>(Command::Digit9);
    if (code >= first && code <= last) {
        return static_cast<int>(code - first);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Binary frame layer
// ---------------------------------------------------------------------------

std::uint32_t packFrame(const NecFrame& frame) noexcept {
    return static_cast<std::uint32_t>(frame.address) |
           (static_cast<std::uint32_t>(inverted(frame.address)) << 8) |
           (static_cast<std::uint32_t>(frame.command) << 16) |
           (static_cast<std::uint32_t>(inverted(frame.command)) << 24);
}

std::optional<NecFrame> unpackFrame(std::uint32_t word) noexcept {
    const auto address = static_cast<std::uint8_t>(word);
    const auto addressInv = static_cast<std::uint8_t>(word >> 8);
    const auto command = static_cast<std::uint8_t>(word >> 16);
    const auto commandInv = static_cast<std::uint8_t>(word >> 24);

    if (addressInv != inverted(address) || commandInv != inverted(command)) {
        return std::nullopt;
    }
    return NecFrame{address, command};
}

std::string toBitString(std::uint32_t word) {
    std::string bits;
    bits.reserve(nec::kBitsPerFrame + 3);
    for (std::size_t i = 0; i < nec::kBitsPerFrame; ++i) {
        if (i != 0 && i % 8 == 0) {
            bits.push_back(' ');
        }
        bits.push_back(((word >> i) & 1U) != 0 ? '1' : '0');
    }
    return bits;
}

std::uint64_t PulseTrain::totalDurationUs() const noexcept {
    return std::accumulate(durationsUs.begin(), durationsUs.end(), std::uint64_t{0});
}

// ---------------------------------------------------------------------------
// Timing layer
// ---------------------------------------------------------------------------

IrProtocolEncoder::IrProtocolEncoder(std::uint8_t deviceAddress) noexcept
    : address_(deviceAddress) {}

NecFrame IrProtocolEncoder::makeFrame(Command command) const noexcept {
    return NecFrame{address_, static_cast<std::uint8_t>(command)};
}

PulseTrain IrProtocolEncoder::encode(Command command) const {
    return encodeFrame(makeFrame(command));
}

PulseTrain IrProtocolEncoder::encodeFrame(const NecFrame& frame) {
    PulseTrain train;
    train.durationsUs.reserve(nec::kFramePulseCount);

    train.durationsUs.push_back(nec::kLeaderMarkUs);
    train.durationsUs.push_back(nec::kLeaderSpaceUs);

    const std::uint32_t word = packFrame(frame);
    for (std::size_t i = 0; i < nec::kBitsPerFrame; ++i) {
        const bool bit = ((word >> i) & 1U) != 0;
        train.durationsUs.push_back(nec::kBitMarkUs);
        train.durationsUs.push_back(bit ? nec::kOneSpaceUs : nec::kZeroSpaceUs);
    }

    train.durationsUs.push_back(nec::kTrailingMarkUs);
    return train;
}

PulseTrain IrProtocolEncoder::encodeRepeat() {
    PulseTrain train;
    train.durationsUs = {nec::kLeaderMarkUs, nec::kRepeatSpaceUs, nec::kTrailingMarkUs};
    return train;
}

// ---------------------------------------------------------------------------
// Wire layer
// ---------------------------------------------------------------------------

namespace wire {

std::vector<std::uint8_t> serialize(const PulseTrain& train) {
    if (train.durationsUs.empty() || train.durationsUs.size() > kMaxPulses) {
        throw std::length_error("pulse train size is outside the wire format limits");
    }

    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + train.durationsUs.size() * 4);
    out.insert(out.end(), kMagic.begin(), kMagic.end());
    out.push_back(kWireVersion);
    out.push_back(0);
    appendU16(out, static_cast<std::uint16_t>(train.durationsUs.size()));
    appendU32(out, train.carrierHz);
    for (const auto duration : train.durationsUs) {
        appendU32(out, duration);
    }
    return out;
}

std::optional<PulseTrain> deserialize(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < kHeaderSize) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < kMagic.size(); ++i) {
        if (bytes[i] != kMagic[i]) {
            return std::nullopt;
        }
    }
    if (bytes[4] != kWireVersion) {
        return std::nullopt;
    }

    const std::size_t count = readU16(bytes, 6);
    if (count == 0 || count > kMaxPulses || bytes.size() != kHeaderSize + count * 4) {
        return std::nullopt;
    }

    PulseTrain train;
    train.carrierHz = readU32(bytes, 8);
    train.durationsUs.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        train.durationsUs.push_back(readU32(bytes, kHeaderSize + i * 4));
    }
    return train;
}

} // namespace wire

} // namespace ir
