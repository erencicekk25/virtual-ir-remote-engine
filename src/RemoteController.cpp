#include "RemoteController.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <utility>

namespace remote {

namespace {

struct KeyBinding {
    std::string_view key;
    ir::Command command;
};

constexpr std::array<KeyBinding, 25> kKeyBindings{{
    {"power", ir::Command::Power},       {"pwr", ir::Command::Power},
    {"vol+", ir::Command::VolumeUp},     {"volup", ir::Command::VolumeUp},
    {"vol-", ir::Command::VolumeDown},   {"voldown", ir::Command::VolumeDown},
    {"ch+", ir::Command::ChannelUp},     {"chup", ir::Command::ChannelUp},
    {"ch-", ir::Command::ChannelDown},   {"chdown", ir::Command::ChannelDown},
    {"mute", ir::Command::Mute},         {"input", ir::Command::Input},
    {"source", ir::Command::Input},      {"ok", ir::Command::Ok},
    {"enter", ir::Command::Ok},          {"0", ir::Command::Digit0},
    {"1", ir::Command::Digit1},          {"2", ir::Command::Digit2},
    {"3", ir::Command::Digit3},          {"4", ir::Command::Digit4},
    {"5", ir::Command::Digit5},          {"6", ir::Command::Digit6},
    {"7", ir::Command::Digit7},          {"8", ir::Command::Digit8},
    {"9", ir::Command::Digit9},
}};

constexpr double kMaxJitter = 0.9;

} // namespace

RemoteController::RemoteController(std::uint8_t deviceAddress, net::Endpoint target,
                                   double jitterRatio, std::optional<std::uint32_t> seed)
    : encoder_(deviceAddress),
      target_(std::move(target)),
      rng_(seed ? *seed : std::random_device{}()) {
    setJitter(jitterRatio);
}

void RemoteController::setJitter(double ratio) noexcept {
    jitterRatio_ = std::clamp(ratio, 0.0, kMaxJitter);
}

std::size_t RemoteController::press(ir::Command command, std::size_t holdRepeats) {
    TransmissionEvent event;
    event.command = command;
    event.frame = encoder_.makeFrame(command);
    event.binaryWord = ir::packFrame(event.frame);

    std::size_t sent = transmit(encoder_.encode(command), event);

    const auto framePeriod = std::chrono::microseconds(ir::nec::kFramePeriodUs);
    for (std::size_t i = 0; i < holdRepeats; ++i) {
        std::this_thread::sleep_for(framePeriod);
        TransmissionEvent repeat = event;
        repeat.isRepeat = true;
        sent += transmit(ir::IrProtocolEncoder::encodeRepeat(), repeat);
    }
    return sent;
}

std::size_t RemoteController::transmit(ir::PulseTrain train, TransmissionEvent event) {
    applyJitter(train);
    const auto payload = ir::wire::serialize(train);

    event.pulseCount = train.durationsUs.size();
    event.airtimeUs = train.totalDurationUs();
    event.bytesOnWire = payload.size();

    if (listener_) {
        listener_(event);
    }
    socket_.sendTo(payload, target_);
    return 1;
}

void RemoteController::applyJitter(ir::PulseTrain& train) {
    if (jitterRatio_ <= 0.0) {
        return;
    }
    std::uniform_real_distribution<double> distribution(-jitterRatio_, jitterRatio_);
    for (auto& duration : train.durationsUs) {
        const double noisy = static_cast<double>(duration) * (1.0 + distribution(rng_));
        duration = static_cast<std::uint32_t>(std::max(1.0, std::round(noisy)));
    }
}

std::optional<ir::Command> RemoteController::parseKey(std::string_view key) {
    std::string normalized;
    normalized.reserve(key.size());
    for (const char c : key) {
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    for (const auto& binding : kKeyBindings) {
        if (binding.key == normalized) {
            return binding.command;
        }
    }
    return std::nullopt;
}

} // namespace remote
