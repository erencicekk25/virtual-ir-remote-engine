#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ir {

/// Logical remote control buttons and their NEC command codes.
enum class Command : std::uint8_t {
    ChannelUp   = 0x00,
    ChannelDown = 0x01,
    VolumeUp    = 0x02,
    VolumeDown  = 0x03,
    Power       = 0x08,
    Mute        = 0x09,
    Input       = 0x0B,
    Digit0      = 0x10,
    Digit1      = 0x11,
    Digit2      = 0x12,
    Digit3      = 0x13,
    Digit4      = 0x14,
    Digit5      = 0x15,
    Digit6      = 0x16,
    Digit7      = 0x17,
    Digit8      = 0x18,
    Digit9      = 0x19,
    Ok          = 0x44,
};

[[nodiscard]] std::string_view toString(Command command) noexcept;
[[nodiscard]] std::optional<Command> commandFromCode(std::uint8_t code) noexcept;

/// Commands that keep acting while the button is held down (NEC repeat codes).
[[nodiscard]] bool isRepeatable(Command command) noexcept;

/// Returns the digit value (0-9) for digit commands, std::nullopt otherwise.
[[nodiscard]] std::optional<int> digitValue(Command command) noexcept;

/// NEC protocol timing constants, in microseconds.
namespace nec {
inline constexpr std::uint32_t kCarrierFrequencyHz = 38'000;
inline constexpr std::uint32_t kLeaderMarkUs       = 9'000;
inline constexpr std::uint32_t kLeaderSpaceUs      = 4'500;
inline constexpr std::uint32_t kRepeatSpaceUs      = 2'250;
inline constexpr std::uint32_t kBitMarkUs          = 562;
inline constexpr std::uint32_t kZeroSpaceUs        = 562;
inline constexpr std::uint32_t kOneSpaceUs         = 1'687;
inline constexpr std::uint32_t kTrailingMarkUs     = 562;
inline constexpr std::uint32_t kFramePeriodUs      = 108'000;

inline constexpr std::size_t kBitsPerFrame       = 32;
/// leader mark + leader space + 32 * (mark + space) + trailing mark
inline constexpr std::size_t kFramePulseCount    = 2 + kBitsPerFrame * 2 + 1;
/// leader mark + repeat space + trailing mark
inline constexpr std::size_t kRepeatPulseCount   = 3;
} // namespace nec

/// Logical content of a single NEC frame.
struct NecFrame {
    std::uint8_t address = 0;
    std::uint8_t command = 0;

    friend bool operator==(const NecFrame& lhs, const NecFrame& rhs) noexcept {
        return lhs.address == rhs.address && lhs.command == rhs.command;
    }
};

/// Packs a frame into its 32-bit binary form: address, ~address, command, ~command.
/// Bit 0 of the returned word is the first bit put on the air (NEC is LSB first).
[[nodiscard]] std::uint32_t packFrame(const NecFrame& frame) noexcept;

/// Unpacks a 32-bit binary word, validating the inverted integrity bytes.
[[nodiscard]] std::optional<NecFrame> unpackFrame(std::uint32_t word) noexcept;

/// Renders the word in transmission order, grouped by byte: "00100000 11011111 ...".
[[nodiscard]] std::string toBitString(std::uint32_t word);

/// A modulated IR burst sequence. Durations alternate mark/space and always
/// start (and, for valid NEC traffic, end) with a mark.
struct PulseTrain {
    std::uint32_t carrierHz = nec::kCarrierFrequencyHz;
    std::vector<std::uint32_t> durationsUs;

    [[nodiscard]] std::uint64_t totalDurationUs() const noexcept;
};

/// Converts high-level commands into NEC binary frames and timing sequences.
class IrProtocolEncoder {
public:
    explicit IrProtocolEncoder(std::uint8_t deviceAddress) noexcept;

    [[nodiscard]] std::uint8_t deviceAddress() const noexcept { return address_; }

    [[nodiscard]] NecFrame makeFrame(Command command) const noexcept;
    [[nodiscard]] PulseTrain encode(Command command) const;

    [[nodiscard]] static PulseTrain encodeFrame(const NecFrame& frame);
    [[nodiscard]] static PulseTrain encodeRepeat();

private:
    std::uint8_t address_;
};

/// Wire format used to carry a pulse train inside a UDP datagram.
///
///   offset  size  field
///   0       4     magic "IRPT"
///   4       1     format version (1)
///   5       1     reserved (0)
///   6       2     pulse count (big endian)
///   8       4     carrier frequency in Hz (big endian)
///   12      4*N   durations in microseconds (big endian)
namespace wire {
inline constexpr std::size_t kHeaderSize = 12;
inline constexpr std::size_t kMaxPulses  = 512;
inline constexpr std::size_t kMaxPacketSize = kHeaderSize + kMaxPulses * 4;

[[nodiscard]] std::vector<std::uint8_t> serialize(const PulseTrain& train);
[[nodiscard]] std::optional<PulseTrain> deserialize(const std::vector<std::uint8_t>& bytes);
} // namespace wire

} // namespace ir
