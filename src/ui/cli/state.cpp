#include "state.hpp"

#include "Parser.hpp"
#include "ui/app/SystemState.hpp"

#include <ostream>

namespace ui::cli::state
{
    namespace commands
    {
        using namespace pnm::meta::string::literals;

        [[= "Show UI, IO and combined application states, LED colour and IO freshness. Motor readiness is separate."_fs]]
        auto state(const SystemState& system, const Arguments&, std::ostream& output) -> CallbackResult
        {
            const auto snapshot{ system.snapshot() };
            const auto colour{ snapshot.combined == app::State::Ready ? "green" :
                               snapshot.combined == app::State::Fault ? "red" : "yellow" };
            output << "Combined: " << pnm::meta::enumeration::enumerator_name(snapshot.combined)
                   << " (" << colour << ")\n"
                   << "UI: " << pnm::meta::enumeration::enumerator_name(snapshot.ui) << '\n'
                   << "IO: ";
            if (snapshot.io) {
                output << pnm::meta::enumeration::enumerator_name(*snapshot.io)
                       << (snapshot.io_fresh ? " (fresh)\n" : " (not fresh)\n");
            } else {
                output << "Unavailable\n";
            }
            return output ? CallbackResult{ 0 } : callback_failure("cannot write command output");
        }
    }

    void setup(Parser& parser, const SystemState& system)
    {
        std::call_once(parser.m_stateRegistration, [&parser, &system] {
            register_commands<^^commands>(parser, [&system](auto command) {
                return [&system, command](const Arguments& args, std::ostream& output) {
                    return command(system, args, output);
                };
            });
        });
    }
}
