#include "profiling.hpp"
#include "Parser.hpp"
#include "common/diagnostics/Profiling.hpp"
#include "common/icc/catalog/services.hpp"
#include <chrono>
#include <ostream>
#include <stdexcept>

namespace ui::cli::profiling
{
    namespace commands
    {
        using namespace pnm::meta::string::literals;
        using Service = pnm::msg::Service<diagnostics::ProfileRequest, diagnostics::ProfileResponse>;
        struct Context
        {
            Service io;
            std::stop_token stop;
        };
        [[
            = "Bounded trace capture: status/summary [ui|io|all], arm <system|motion|graphics|memory> [ui|io|all], freeze [ui|io|all]. Arm only with motors disabled; export via scripts/profile.py."_fs,
            = Arg{ .name = "operation"_fs, .description = "status, summary, arm or freeze"_fs },
            = Arg{ .name = "options"_fs,
                   .description = "preset and/or controller"_fs,
                   .optional = true,
                   .variadic = true }
        ]] CallbackResult profile(Context& context, const Arguments& args, std::ostream& out)
        {
            diagnostics::ProfileRequest request{};
            const auto operation = args.at(0);
            std::size_t target_index = 1;
            if (operation == "arm") {
                request.operation = diagnostics::ProfileOperation::Arm;
                if (args.size() < 2)
                    return callback_failure("profile arm requires a preset");
                const auto preset = args.at(1);
                if (preset == "system")
                    request.preset = PROFILE_SYSTEM;
                else if (preset == "motion")
                    request.preset = PROFILE_MOTION;
                else if (preset == "graphics")
                    request.preset = PROFILE_GRAPHICS;
                else if (preset == "memory")
                    request.preset = PROFILE_MEMORY;
                else
                    return callback_failure("preset must be system, motion, graphics or memory");
                target_index = 2;
                request.capture = profile_time_us();
            }
            else if (operation == "freeze")
                request.operation = diagnostics::ProfileOperation::Freeze;
            else if (operation != "status" && operation != "summary")
                return callback_failure("unknown profile operation");
            const auto target = args.size() > target_index ? args.at(target_index) : "all";
            if (args.size() > target_index + 1 || (target != "ui" && target != "io" && target != "all"))
                return callback_failure("controller must be ui, io or all");
            const auto print = [&](const char* name, const diagnostics::ProfileResponse& response) {
                const auto& s = response.status;
                out << name << " profile state=" << s.state << " capture=" << s.capture
                    << " preset=" << s.preset << " result=" << response.result << " reason=" << s.reason
                    << " events=" << s.events << " wraps=" << s.wraps << " filtered=" << s.filtered << '\n'
                    << "  heap_free=" << s.heap_available << " sampled_min_free=" << s.sampled_min_free
                    << " allocations=" << s.allocation_calls << " failures=" << s.allocation_failures
                    << " last_request=" << s.last_requested << " registry_free=" << s.registry_free << '\n';
                if (s.state == PROFILE_UNAVAILABLE)
                    out << "  Detailed recording requires a profiling build.\n";
                if (response.result == 5)
                    out << "  Disable motors before arming a capture.\n";
                if (response.result == 3)
                    out << "  Freeze the existing capture before rearming.\n";
                if (s.failure.failure_count)
                    out << "  failure reason=" << s.failure.reason << " operation=" << s.failure.operation
                        << " bytes=" << s.failure.size << " caller=" << std::hex << s.failure.caller
                        << std::dec << '\n';
            };
            if (target != "ui") {
                auto pending = context.io.request(request, std::chrono::seconds{ 2 }, context.stop);
                const auto response = pending.get();
                if (!response)
                    return callback_failure("IO profiling unavailable; use SWD export if ICC failed");
                print("IO", *response);
                if (response->result)
                    return callback_failure("IO rejected profile operation");
                if (target == "all" && response->status.core == 0) {
                    out << "Linux UI/IO share this recorder and runtime.\n";
                    return 0;
                }
            }
            if (target != "io") {
                const auto response = diagnostics::controlProfile(request);
                print("UI", response);
                if (response.result)
                    return callback_failure("UI rejected profile operation");
            }
            return 0;
        }
    }
    void setup(Parser& parser, pnm::msg::Bus& bus, std::stop_token stop)
    {
        std::call_once(parser.m_profileRegistration, [&] {
            auto context =
              std::make_shared<commands::Context>(bus.service<^^icc::Services::io_profile>(), stop);
            register_commands<^^commands>(parser, [context](auto command) {
                return [context, command](const Arguments& args, std::ostream& out) {
                    return command(*context, args, out);
                };
            });
        });
    }
}
