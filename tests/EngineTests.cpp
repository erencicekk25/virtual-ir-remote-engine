// Minimal self-contained test runner: no third-party framework required.

#include "IrProtocolEncoder.hpp"
#include "RemoteController.hpp"
#include "VirtualTvDevice.hpp"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define EXPECT_TRUE(condition)                                                              \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            ++g_failures;                                                                   \
            std::cerr << "    FAILED: " #condition " (" << __FILE__ << ':' << __LINE__ << ")\n"; \
        }                                                                                   \
    } while (false)

#define EXPECT_EQ(actual, expected) EXPECT_TRUE((actual) == (expected))

struct TestCase {
    const char* name;
    std::function<void()> body;
};

const std::vector<ir::Command> kAllCommands{
    ir::Command::Power,  ir::Command::VolumeUp, ir::Command::VolumeDown, ir::Command::Mute,
    ir::Command::ChannelUp, ir::Command::ChannelDown, ir::Command::Input, ir::Command::Ok,
    ir::Command::Digit0, ir::Command::Digit1, ir::Command::Digit2, ir::Command::Digit3,
    ir::Command::Digit4, ir::Command::Digit5, ir::Command::Digit6, ir::Command::Digit7,
    ir::Command::Digit8, ir::Command::Digit9,
};

void testFramePacking() {
    const ir::NecFrame frame{0x04, 0x08};
    const std::uint32_t word = ir::packFrame(frame);
    EXPECT_EQ(word, 0xF708FB04u);

    const auto decoded = ir::unpackFrame(word);
    EXPECT_TRUE(decoded.has_value());
    EXPECT_TRUE(decoded && *decoded == frame);

    EXPECT_TRUE(!ir::unpackFrame(word ^ 0x00000100u).has_value()); // corrupted ~address
    EXPECT_TRUE(!ir::unpackFrame(word ^ 0x80000000u).has_value()); // corrupted ~command
}

void testBitString() {
    EXPECT_EQ(ir::toBitString(0x000000FFu), std::string{"11111111 00000000 00000000 00000000"});
    EXPECT_EQ(ir::toBitString(0x00000001u), std::string{"10000000 00000000 00000000 00000000"});
}

void testFrameTiming() {
    const ir::IrProtocolEncoder encoder(0x04);
    const auto train = encoder.encode(ir::Command::Power);

    EXPECT_EQ(train.durationsUs.size(), ir::nec::kFramePulseCount);
    EXPECT_EQ(train.durationsUs.front(), ir::nec::kLeaderMarkUs);
    EXPECT_EQ(train.durationsUs[1], ir::nec::kLeaderSpaceUs);
    EXPECT_EQ(train.durationsUs.back(), ir::nec::kTrailingMarkUs);
    EXPECT_EQ(train.carrierHz, ir::nec::kCarrierFrequencyHz);

    // Every NEC frame carries exactly 16 ones (each byte is paired with its inverse),
    // so the airtime is constant: 9000 + 4500 + 32*562 + 16*1687 + 16*562 + 562.
    const std::uint64_t expected = 9000 + 4500 + 32 * 562 + 16 * 1687 + 16 * 562 + 562;
    for (const auto command : kAllCommands) {
        EXPECT_EQ(encoder.encode(command).totalDurationUs(), expected);
    }
}

void testEncodeParseRoundTrip() {
    const ir::IrProtocolEncoder encoder(0x04);
    const tv::IrSignalParser parser;
    for (const auto command : kAllCommands) {
        const auto result = parser.parse(encoder.encode(command));
        EXPECT_TRUE(result.status == tv::DecodeStatus::Frame);
        EXPECT_EQ(result.frame.address, 0x04);
        EXPECT_TRUE(ir::commandFromCode(result.frame.command) == command);
    }

    const auto repeat = parser.parse(ir::IrProtocolEncoder::encodeRepeat());
    EXPECT_TRUE(repeat.status == tv::DecodeStatus::Repeat);
}

void testParserRejectsBadSignals() {
    const tv::IrSignalParser parser;
    const ir::IrProtocolEncoder encoder(0x04);

    auto wrongLeader = encoder.encode(ir::Command::Mute);
    wrongLeader.durationsUs[0] = 4'000;
    EXPECT_TRUE(parser.parse(wrongLeader).status == tv::DecodeStatus::TimingError);

    auto truncated = encoder.encode(ir::Command::Mute);
    truncated.durationsUs.resize(41);
    EXPECT_TRUE(parser.parse(truncated).status == tv::DecodeStatus::TimingError);

    auto wrongCarrier = encoder.encode(ir::Command::Mute);
    wrongCarrier.carrierHz = 56'000;
    EXPECT_TRUE(parser.parse(wrongCarrier).status == tv::DecodeStatus::TimingError);

    // Flip a single command bit (bit 16) from 0 to 1: timings are valid, integrity is not.
    auto flipped = encoder.encode(ir::Command::Power);
    const std::size_t spaceIndex = 3 + 16 * 2;
    flipped.durationsUs[spaceIndex] = flipped.durationsUs[spaceIndex] == ir::nec::kOneSpaceUs
                                          ? ir::nec::kZeroSpaceUs
                                          : ir::nec::kOneSpaceUs;
    EXPECT_TRUE(parser.parse(flipped).status == tv::DecodeStatus::IntegrityError);
}

void testParserTolerance() {
    const tv::IrSignalParser parser(0.25);
    auto train = ir::IrProtocolEncoder(0x04).encode(ir::Command::VolumeUp);
    for (std::size_t i = 0; i < train.durationsUs.size(); ++i) {
        // Alternate +20% / -20% skew: inside the 25% tolerance window.
        const double factor = (i % 2 == 0) ? 1.20 : 0.80;
        train.durationsUs[i] = static_cast<std::uint32_t>(train.durationsUs[i] * factor);
    }
    EXPECT_TRUE(parser.parse(train).status == tv::DecodeStatus::Frame);
}

void testWireFormat() {
    const auto train = ir::IrProtocolEncoder(0x7A).encode(ir::Command::Input);
    const auto bytes = ir::wire::serialize(train);
    EXPECT_EQ(bytes.size(), ir::wire::kHeaderSize + train.durationsUs.size() * 4);

    const auto restored = ir::wire::deserialize(bytes);
    EXPECT_TRUE(restored.has_value());
    EXPECT_TRUE(restored && restored->durationsUs == train.durationsUs);
    EXPECT_TRUE(restored && restored->carrierHz == train.carrierHz);

    auto badMagic = bytes;
    badMagic[0] = 'X';
    EXPECT_TRUE(!ir::wire::deserialize(badMagic).has_value());

    auto truncated = bytes;
    truncated.pop_back();
    EXPECT_TRUE(!ir::wire::deserialize(truncated).has_value());

    EXPECT_TRUE(!ir::wire::deserialize({}).has_value());
}

void testStateMachine() {
    tv::TvStateMachine machine;
    EXPECT_TRUE(machine.state().power == tv::PowerState::Standby);

    machine.apply(ir::Command::VolumeUp);
    EXPECT_EQ(machine.state().volume, 20); // ignored in standby

    machine.apply(ir::Command::Power);
    EXPECT_TRUE(machine.state().power == tv::PowerState::On);

    machine.apply(ir::Command::VolumeUp);
    machine.applyRepeat();
    machine.applyRepeat();
    EXPECT_EQ(machine.state().volume, 23);

    machine.apply(ir::Command::Mute);
    EXPECT_TRUE(machine.state().muted);
    machine.applyRepeat(); // Mute is not repeatable
    EXPECT_TRUE(machine.state().muted);
    machine.apply(ir::Command::VolumeDown);
    EXPECT_TRUE(!machine.state().muted);
    EXPECT_EQ(machine.state().volume, 22);

    machine.apply(ir::Command::Digit4);
    machine.apply(ir::Command::Digit2);
    EXPECT_EQ(machine.state().pendingDigits, std::string{"42"});
    machine.apply(ir::Command::Ok);
    EXPECT_EQ(machine.state().channel, 42);

    machine.apply(ir::Command::Digit1);
    machine.apply(ir::Command::Digit2);
    machine.apply(ir::Command::Digit3); // third digit auto-commits
    EXPECT_EQ(machine.state().channel, 123);

    machine.apply(ir::Command::Digit0);
    machine.apply(ir::Command::Ok); // channel 0 does not exist
    EXPECT_EQ(machine.state().channel, 123);

    machine.apply(ir::Command::Input);
    EXPECT_TRUE(machine.state().input == tv::InputSource::Hdmi1);

    machine.apply(ir::Command::Power);
    EXPECT_TRUE(machine.state().power == tv::PowerState::Standby);
}

void testVolumeBounds() {
    tv::TvStateMachine machine;
    machine.apply(ir::Command::Power);
    machine.apply(ir::Command::VolumeDown);
    for (int i = 0; i < 50; ++i) {
        machine.applyRepeat();
    }
    EXPECT_EQ(machine.state().volume, tv::TvState::kMinVolume);
}

void testEndToEndOverUdp() {
    tv::VirtualTvDevice device(0x04);
    device.start();

    remote::RemoteController remoteControl(0x04, device.endpoint(), 0.0, 42u);

    std::uint64_t expected = remoteControl.press(ir::Command::Power);
    EXPECT_TRUE(device.waitForProcessed(expected, std::chrono::seconds(2)));
    EXPECT_TRUE(device.snapshot().power == tv::PowerState::On);

    expected += remoteControl.press(ir::Command::VolumeUp, 3);
    EXPECT_TRUE(device.waitForProcessed(expected, std::chrono::seconds(2)));
    EXPECT_EQ(device.snapshot().volume, 24);

    // A frame addressed to a different device must be ignored.
    remoteControl.setDeviceAddress(0x20);
    expected += remoteControl.press(ir::Command::Power);
    EXPECT_TRUE(device.waitForProcessed(expected, std::chrono::seconds(2)));
    EXPECT_TRUE(device.snapshot().power == tv::PowerState::On);

    // Moderate jitter stays inside the decoder tolerance.
    remoteControl.setDeviceAddress(0x04);
    remoteControl.setJitter(0.15);
    expected += remoteControl.press(ir::Command::ChannelUp);
    EXPECT_TRUE(device.waitForProcessed(expected, std::chrono::seconds(2)));
    EXPECT_EQ(device.snapshot().channel, 2);

    device.stop();
}

void testRaiiRestart() {
    // Construct and destroy several devices back to back: sockets and worker
    // threads must be released deterministically every time.
    for (int i = 0; i < 5; ++i) {
        tv::VirtualTvDevice device(0x01);
        device.start();
    }
    EXPECT_TRUE(true);
}

void testKeyParsing() {
    EXPECT_TRUE(remote::RemoteController::parseKey("POWER") == ir::Command::Power);
    EXPECT_TRUE(remote::RemoteController::parseKey("vol+") == ir::Command::VolumeUp);
    EXPECT_TRUE(remote::RemoteController::parseKey("7") == ir::Command::Digit7);
    EXPECT_TRUE(!remote::RemoteController::parseKey("teleport").has_value());
}

} // namespace

int main() {
    const std::vector<TestCase> tests{
        {"frame packing", testFramePacking},
        {"bit string", testBitString},
        {"frame timing", testFrameTiming},
        {"encode/parse round trip", testEncodeParseRoundTrip},
        {"parser rejects bad signals", testParserRejectsBadSignals},
        {"parser tolerance", testParserTolerance},
        {"wire format", testWireFormat},
        {"state machine", testStateMachine},
        {"volume bounds", testVolumeBounds},
        {"key parsing", testKeyParsing},
        {"RAII restart", testRaiiRestart},
        {"end-to-end over UDP", testEndToEndOverUdp},
    };

    for (const auto& test : tests) {
        const int before = g_failures;
        try {
            test.body();
        } catch (const std::exception& error) {
            ++g_failures;
            std::cerr << "    EXCEPTION: " << error.what() << '\n';
        }
        std::cout << (g_failures == before ? "[ OK ] " : "[FAIL] ") << test.name << '\n';
    }

    std::cout << (g_failures == 0 ? "All tests passed.\n" : "Some tests failed.\n");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
