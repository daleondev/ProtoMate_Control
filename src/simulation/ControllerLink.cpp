#include "ControllerLink.hpp"
#include "Catalog.hpp"
#include "SharedTransport.hpp"
#include "common/icc/bridge/Bridge.hpp"
#include "runtime/thread.hpp"
#include "simulation/Client.hpp"
#include "ui/cli/Parser.hpp"
#include <cmath>
#include <map>
#include <ostream>
namespace simulation
{
    namespace
    {
        pnm::msg::Bus* active_bus{};
    }
    struct ControllerLink::Impl
    {
        pnm::msg::Bus bus;
        pnm::coro::Context context;
        SharedTransport transport{ *client::memory(), 1, context };
        std::jthread runner;
        icc::Bridge bridge{ bus,
                            context,
                            transport,
                            { .heartbeat = std::chrono::milliseconds{ 250 },
                              .timeout = std::chrono::seconds{ 10 } } };
        Impl()
        {
            runner = runtime::thread::create_jthread({ .name = "sim-icc", .stack_size = 65536 },
                                                     [this](std::stop_token token) {
                std::stop_callback stop{ token, [this] { context.stop(); } };
                context.run();
            });
            bridge.registerServices<Catalog>(Provider::Controller);
            bridge.start();
            active_bus = &bus;
        }
        ~Impl()
        {
            active_bus = nullptr;
            bridge.stop(std::chrono::milliseconds{ 100 });
        }
    };
    ControllerLink::ControllerLink()
      : m_impl{ std::make_unique<Impl>() }
    {
    }
    ControllerLink::~ControllerLink() = default;
    void setupCommands(ui::cli::Parser& parser)
    {
        const auto registration{ parser.registerCommand(
          { .name = "sim",
            .description = "Native hardware simulator controls; mechanical state is provisional",
            .arguments = { { .name = "operation", .description = "status, power, fault" },
                           { .name = "values",
                             .description = "power on|off; fault m1|m2|m3 name [slip-degrees]",
                             .optional = true,
                             .variadic = true } },
            .flags = {},
            .callback = [](const ui::cli::Arguments& args, std::ostream& out) -> ui::cli::CallbackResult {
            if (!active_bus)
                return ui::cli::callback_failure("simulator is unavailable");
            Packet q{ .operation = Operation::State };
            if (args.at(0) == "power") {
                if (args.size() != 2 || (args.at(1) != "on" && args.at(1) != "off"))
                    return ui::cli::callback_failure("usage: sim power on|off");
                q.operation = Operation::Power;
                q.arguments[0] = args.at(1) == "on";
            }
            else if (args.at(0) == "fault") {
                static const std::map<std::string_view, Fault> names{
                    { "clear", Fault::None },
                    { "alarm", Fault::DriverAlarm },
                    { "reference-stuck", Fault::ReferenceStuck },
                    { "reference-open", Fault::ReferenceDisconnected },
                    { "encoder-open", Fault::EncoderDisconnected },
                    { "lost-step", Fault::LostStep },
                    { "slip", Fault::ShaftSlip },
                    { "dma-delay", Fault::DmaDelay },
                    { "irq-delay", Fault::InterruptDelay },
                    { "uart-open", Fault::UartDisconnected },
                    { "uart-corrupt", Fault::UartCorrupt }
                };
                if (args.size() < 3 || args.at(1).size() != 2 || args.at(1)[0] != 'm' ||
                    args.at(1)[1] < '1' || args.at(1)[1] > '3' || !names.contains(args.at(2)))
                    return ui::cli::callback_failure(
                      "usage: sim fault m1|m2|m3 "
                      "clear|alarm|reference-stuck|reference-open|encoder-open|lost-step|slip|dma-delay|irq-"
                      "delay|uart-open|uart-corrupt [degrees]");
                q.operation = Operation::Fault;
                q.arguments[0] = args.at(1)[1] - '1';
                q.arguments[1] = std::to_underlying(names.at(args.at(2)));
                if (args.at(2) == "slip") {
                    if (args.size() != 4)
                        return ui::cli::callback_failure("slip requires signed shaft degrees");
                    std::size_t consumed{};
                    const auto value{ std::stod(std::string{ args.at(3) }, &consumed) };
                    if (consumed != args.at(3).size() || !std::isfinite(value) || std::abs(value) > 36000)
                        return ui::cli::callback_failure("invalid slip");
                    q.arguments[2] = std::bit_cast<std::uint64_t>(value);
                }
            }
            else if (args.at(0) != "status")
                return ui::cli::callback_failure("usage: sim status | sim power on|off | sim fault ...");
            auto pending{ active_bus->service<^^Catalog::control>().request(q, std::chrono::seconds{ 3 }) };
            auto reply{ pending.get() };
            if (!reply || reply->status)
                return ui::cli::callback_failure(
                  "simulator request failed; outcome must be reconciled with sim status");
            if (q.operation == Operation::State) {
                out << "cad-provisional-v1 (NOT calibrated) time=" << reply->time_ns / 1e9
                    << " s enable=" << reply->arguments[2] << " power=" << reply->arguments[3]
                    << " references=" << reply->arguments[0] << " faults=" << reply->arguments[1] << '\n';
                for (unsigned i{}; i < 3; ++i)
                    out << "m" << i + 1 << " shaft=" << std::bit_cast<double>(get(reply->data, i * 8, 8))
                        << " deg; joint=" << std::bit_cast<double>(get(reply->data, 24 + i * 8, 8))
                        << (i == 2 ? " mm" : " rad") << "; TCP " << "XYZ"[i] << "="
                        << std::bit_cast<double>(get(reply->data, 48 + i * 8, 8))
                        << " mm; rejected pulses=" << get(reply->data, 72 + i * 8, 8) << "\n";
            }
            else
                out << "Simulator change acknowledged at " << reply->time_ns / 1e9 << " s.\n";
            return {};
        } }) };
        if (!registration)
            throw std::runtime_error{ registration.error() };
    }
}
