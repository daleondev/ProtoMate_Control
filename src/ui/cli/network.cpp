#include "network.hpp"
#include "Parser.hpp"
#include "ui/opcua/Network.hpp"
#include "ui/opcua/Server.hpp"
#include "ui/app/Services.hpp"
#include "hal/console.hpp"
#include <array>
#include <chrono>
#include <istream>
#include <ostream>
#include <thread>

namespace ui::cli::network
{
    namespace commands
    {
        using namespace pnm::meta::string::literals;
        struct Context { opcua::Network& network; opcua::Server& server; Services& services; std::stop_token stop; };
        // The command owns the CLI reader while prompting. Parser CommandIO
        // is pipeline input, deliberately never an interactive password source.
        template<std::size_t N> bool read_line(std::stop_token stop, std::array<char, N>& value)
        {
            std::size_t count{};
            bool overflow{};
            const auto deadline{ std::chrono::steady_clock::now() + std::chrono::seconds{ 60 } };
            while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                const auto input{ hal::console::try_read() };
                if (input.state == hal::console::InputState::Idle) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{ 10 });
                    continue;
                }
                if (input.state != hal::console::InputState::Character) return false;
                const char c{ input.character };
                // Ignore a remaining CRLF after the preceding prompt line.
                if (c == '\n' && count == 0) continue;
                if (c == '\r' || c == '\n') return !overflow;
                if (c == '\x03') return false;
                if (c == '\b' || c == '\x7f') { if (count) value[--count] = 0; continue; }
                if (static_cast<unsigned char>(c) < 32) return false;
                if (count + 1 >= value.size()) overflow = true;
                else value[count++] = c;
            }
            return false;
        }
        [[= "Network status, setup, connect, disconnect or forget. Setup reads the password privately; never pass it as an argument."_fs,
          = Arg{ .name = "operation"_fs, .description = "status|setup|connect|disconnect|forget|scan|scan-results"_fs }]]
        CallbackResult network(Context& context, const Arguments& args, CommandIO& io)
        {
            const auto operation{ args.at(0) };
            if (operation == "scan-results") {
                const auto scan{context.network.scanStatus()};
                io.output << "scan running=" << scan.running << " result=" << scan.result << " count=" << scan.count << " omitted=" << scan.omitted << '\n';
                for (unsigned i{}; i < scan.count; ++i) {
                    const auto& row{scan.entries[i]};
                    io.output << i + 1 << ": " << row.ssid.data() << " channel=" << row.channel << " signal=" << row.signal << " dBm\n";
                }
                return 0;
            }
            if (operation == "status") {
                const auto s{ context.network.status() };
                io.output << "network state=" << static_cast<unsigned>(s.state) << " result=" << s.result
                          << " busy=" << s.busy << " saved=" << s.saved << " ssid=" << s.ssid.data() << '\n';
                if (s.address) io.output << "address=" << (s.address >> 24) << '.' << ((s.address >> 16) & 255)
                    << '.' << ((s.address >> 8) & 255) << '.' << (s.address & 255) << " port=4840\n";
                const auto d{context.network.diagnostics()};
                io.output << "network memory=" << d.used << '/' << d.capacity << " peak=" << d.peak << " failures=" << d.failures
                    << "; TX packets=" << d.available_packets << '/' << d.packets << " empty=" << d.empty_packet_requests
                    << "; RX packets=" << d.receive_available << '/' << d.receive_packets << " empty=" << d.receive_empty_requests << '\n';
                return 0;
            }
            if (operation == "setup") {
#if !defined(CORE_CM7)
                return callback_failure("Linux uses the host network; configure Wi-Fi in the operating system");
#else
                std::array<char, 33> ssid{};
                std::array<char, 64> password{};
                io.output << "SSID: " << std::flush;
                bool valid{ read_line(context.stop, ssid) };
                if (valid) {
                    io.output << "\nWPA2 password (hidden): " << std::flush;
                    valid = read_line(context.stop, password);
                }
                const bool accepted{ valid && context.network.configure(ssid.data(), password.data(), true) };
                for (auto& c : password) { auto* byte{ static_cast<volatile char*>(&c) }; *byte = 0; }
                io.output << '\n';
                if (!accepted) return callback_failure("Invalid credentials or network manager busy");
                io.output << "Connection requested; successful credentials will be saved locally.\n";
                return 0;
#endif
            }
            const bool accepted{ operation == "connect" ? context.network.connect() :
                operation == "scan" ? context.network.scan() :
                operation == "disconnect" ? context.network.disconnect() :
                operation == "forget" ? context.network.forget() : false };
            if (!accepted) return callback_failure("Unknown network operation or manager busy");
            io.output << "Network request queued.\n";
            return 0;
        }
        [[= "OPC UA status and local remote-command permission. Permission starts OFF after every boot."_fs,
          = Arg{ .name = "operation"_fs, .description = "status|remote|start|stop|restart"_fs },
          = Arg{ .name = "permission"_fs, .description = "on|off (for remote)"_fs, .optional = true }]]
        CallbackResult opcua(Context& context, const Arguments& args, std::ostream& out)
        {
            if (args.at(0) == "remote") {
                if (args.size() != 2 || (args.at(1) != "on" && args.at(1) != "off")) return callback_failure("Expected opcua remote on|off");
                context.services.remotePermission(args.at(1) == "on");
                out << "Remote command permission " << (context.services.remotePermission() ? "ON" : "OFF; remote work stopping") << '\n';
                return 0;
            }
            if (args.at(0) == "start" || args.at(0) == "stop" || args.at(0) == "restart") {
                context.server.enabled(args.at(0) != "stop");
                out << "Server request queued; remote command permission OFF.\n";
                return 0;
            }
            if (args.at(0) != "status") return callback_failure("Expected opcua status|start|stop|restart or opcua remote on|off");
            const auto s{ context.server.status() };
            out << "opcua running=" << s.running << " storage_verified=" << s.storage_verified
                << " error=" << s.error << " capacity=" << s.capacity << " used=" << s.used
                << " remote_permission=" << context.services.remotePermission() << " peak=" << s.peak << " failures=" << s.failures << " startup_ms=" << s.startup_ms << " iterations=" << s.iterations << '\n';
            if (s.failures) out << "opcua allocation failure: requested=" << s.failed_size
                << " largest_free=" << s.largest_free_at_failure << '\n';
            if (s.log_sequence) out << "opcua diagnostic #" << s.log_sequence << ": " << s.message.data() << '\n';
            return 0;
        }
    }
    void setup(Parser& parser, opcua::Network& network, opcua::Server& server, Services& services, std::stop_token stop)
    {
        parser.registerModule(Parser::Module::Network, [&] {
            auto context{ std::make_shared<commands::Context>(network, server, services, stop) };
            register_commands<^^commands>(parser, [context](auto command) {
                return [context, command](const Arguments& args, CommandIO& io) {
                    if constexpr (std::invocable<decltype(command), commands::Context&, const Arguments&, CommandIO&>)
                        return command(*context, args, io);
                    else return command(*context, args, io.output);
                };
            });
        });
    }
}
