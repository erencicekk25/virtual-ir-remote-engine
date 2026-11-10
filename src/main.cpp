#include "IrProtocolEncoder.hpp"
#include "RemoteController.hpp"
#include "VirtualTvDevice.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint8_t kDefaultTvAddress = 0x04;
constexpr double kDefaultJitter = 0.05;
constexpr std::size_t kMaxHoldRepeats = 50;

/// Serialises console output coming from the CLI thread and the TV worker thread.
class Console {
public:
    template <typename... Args>
    static void line(const Args&... args) {
        std::ostringstream out;
        (out << ... << args);
        std::lock_guard<std::mutex> lock(mutex());
        std::cout << out.str() << '\n' << std::flush;
    }

private:
    static std::mutex& mutex() {
        static std::mutex instance;
        return instance;
    }
};

std::string hex8(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

std::string hex2(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << value;
    return out.str();
}

std::string formatMs(std::uint64_t microseconds) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << static_cast<double>(microseconds) / 1000.0 << " ms";
    return out.str();
}

void printTransmission(const remote::TransmissionEvent& event) {
    if (event.isRepeat) {
        Console::line("  [REMOTE] repeat code  pulses=", event.pulseCount,
                      " airtime=", formatMs(event.airtimeUs), " bytes=", event.bytesOnWire);
        return;
    }
    Console::line("  [REMOTE] ", ir::toString(event.command),
                  "  addr=", hex2(event.frame.address), " cmd=", hex2(event.frame.command),
                  " word=", hex8(event.binaryWord));
    Console::line("           bits(LSB first)=", ir::toBitString(event.binaryWord));
    Console::line("           pulses=", event.pulseCount, " airtime=", formatMs(event.airtimeUs),
                  " bytes=", event.bytesOnWire);
}

void printHelp() {
    Console::line(
        "Keys:\n"
        "  power | vol+ | vol- | mute | ch+ | ch- | input | ok | 0-9\n"
        "  Append a number to hold the button, e.g. 'vol+ 5' sends one frame + 5 repeat codes.\n"
        "Commands:\n"
        "  status            show the TV state\n"
        "  jitter <ratio>    timing noise added by the remote (0.0 - 0.9)\n"
        "  address <hex>     change the address the remote targets (TV listens on ",
        hex2(kDefaultTvAddress), ")\n"
        "  help              show this help\n"
        "  quit              exit");
}

struct Options {
    bool demo = false;
    double jitter = kDefaultJitter;
    std::uint16_t port = 0;
};

std::optional<Options> parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--demo") {
            options.demo = true;
        } else if (arg == "--jitter" && i + 1 < argc) {
            options.jitter = std::strtod(argv[++i], nullptr);
        } else if (arg == "--port" && i + 1 < argc) {
            options.port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: virtual_ir_remote [--demo] [--jitter <ratio>] [--port <udp-port>]\n";
            return std::nullopt;
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            return std::nullopt;
        }
    }
    return options;
}

/// Ties the remote and the TV together and interprets CLI lines.
class Session {
public:
    Session(remote::RemoteController& remote, tv::VirtualTvDevice& device)
        : remote_(remote), device_(device) {}

    /// Returns false when the user asked to quit.
    bool execute(const std::string& line) {
        std::istringstream input(line);
        std::string verb;
        if (!(input >> verb)) {
            return true;
        }

        if (verb == "quit" || verb == "exit") {
            return false;
        }
        if (verb == "help") {
            printHelp();
        } else if (verb == "status") {
            Console::line("  [TV]     ", tv::describe(device_.snapshot()));
        } else if (verb == "jitter") {
            double ratio = 0.0;
            if (input >> ratio) {
                remote_.setJitter(ratio);
            }
            Console::line("  remote jitter = +/-", remote_.jitter() * 100.0, "%");
        } else if (verb == "address") {
            std::string value;
            if (input >> value) {
                remote_.setDeviceAddress(
                    static_cast<std::uint8_t>(std::strtoul(value.c_str(), nullptr, 16)));
            }
            Console::line("  remote targets address ", hex2(remote_.deviceAddress()));
        } else if (const auto command = remote::RemoteController::parseKey(verb)) {
            std::size_t hold = 0;
            input >> hold;
            sendAndWait(*command, std::min(hold, kMaxHoldRepeats));
        } else {
            Console::line("  unknown input '", verb, "' (type 'help')");
        }
        return true;
    }

private:
    void sendAndWait(ir::Command command, std::size_t hold) {
        const std::uint64_t before = device_.processedPackets();
        const std::size_t sent = remote_.press(command, hold);
        if (!device_.waitForProcessed(before + sent, std::chrono::seconds(2))) {
            Console::line("  warning: TV did not acknowledge all packets in time");
        }
    }

    remote::RemoteController& remote_;
    tv::VirtualTvDevice& device_;
};

const std::vector<std::string>& demoScript() {
    static const std::vector<std::string> script{
        "vol+",          // ignored: TV is in standby
        "power",         // STANDBY -> ON
        "vol+ 4",        // one frame + four repeat codes
        "mute",
        "vol-",          // un-mutes and lowers the volume
        "4", "2", "ok",  // direct channel entry
        "ch+ 2",
        "input", "input",
        "address 0x20",  // the remote now talks to another device...
        "power",         // ...so the TV ignores this frame
        "address 0x04",
        "jitter 0.20",   // noisy but still within the 25% decoder tolerance
        "ch-",
        "jitter 0.70",   // far too noisy: expect a decode failure
        "ch-",
        "jitter 0.05",
        "status",
        "power",
    };
    return script;
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parseOptions(argc, argv);
    if (!options) {
        return EXIT_SUCCESS;
    }

    try {
        tv::VirtualTvDevice device(
            kDefaultTvAddress, options->port,
            [](const std::string& message) { Console::line("  [TV]     ", message); });
        device.start();

        remote::RemoteController remoteControl(kDefaultTvAddress, device.endpoint(), options->jitter);
        remoteControl.setTransmitListener(printTransmission);

        Session session(remoteControl, device);

        Console::line("virtual-ir-remote-engine | NEC @ 38 kHz over UDP 127.0.0.1:",
                      device.endpoint().port, " | TV address ", hex2(kDefaultTvAddress),
                      " | jitter +/-", remoteControl.jitter() * 100.0, "%");

        if (options->demo) {
            for (const auto& line : demoScript()) {
                Console::line("\n> ", line);
                session.execute(line);
            }
        } else {
            printHelp();
            std::string line;
            while (true) {
                std::cout << "\n> " << std::flush;
                if (!std::getline(std::cin, line) || !session.execute(line)) {
                    break;
                }
            }
        }

        device.stop();
        Console::line("\nShutting down. Final state: ", tv::describe(device.snapshot()));
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
