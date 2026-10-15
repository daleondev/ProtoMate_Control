#include "display.hpp"
#include "Parser.hpp"
#include "ui/gui/Controller.hpp"
#include <charconv>
#include <ostream>
namespace ui::cli::display {
    namespace commands {
        using namespace pnm::meta::string::literals;
        [[= "Show RGB color bars through the GUI renderer."_fs, = Name{"display bars"_fs}]]
        auto bars(ui::gui::Controller& gui, const Arguments&, std::ostream&) -> CallbackResult {
            return gui.bars() ? CallbackResult{0} : callback_failure("GUI unavailable or request queue full");
        }
        [[= "Return to the shared GUI showcase."_fs, = Name{"display gui"_fs}]]
        auto showcase(ui::gui::Controller& gui, const Arguments&, std::ostream&) -> CallbackResult {
            return gui.showcase() ? CallbackResult{0} : callback_failure("GUI unavailable or request queue full");
        }
        [[= "Set display backlight (0..100 percent)."_fs, = Name{"display backlight"_fs},
          = Arg{.name="percent"_fs, .description="0..100"_fs}]]
        auto backlight(ui::gui::Controller& gui, const Arguments& args, std::ostream&) -> CallbackResult {
            unsigned percent{}; const auto text = args.require("percent");
            const auto [end, error] = std::from_chars(text.data(), text.data()+text.size(), percent);
            if (error != std::errc{} || end != text.data()+text.size() || percent > 100) { return callback_failure("Expected 0..100"); }
            return gui.backlight(percent) ? CallbackResult{0} : callback_failure("GUI unavailable or request queue full");
        }
        [[= "Report cached touch state; performs no I2C transaction."_fs, = Name{"display touch"_fs}]]
        auto touch(ui::gui::Controller& gui, const Arguments&, std::ostream& out) -> CallbackResult {
            const auto status = gui.status();
            out << "Touch " << (status.touchAvailable ? "available" : "unavailable") << "; contact=" << status.touchDown
                << "; x=" << status.touchX << "; y=" << status.touchY;
            if (status.touchTime != std::chrono::steady_clock::time_point{}) {
                out << "; age=" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-status.touchTime).count() << " ms";
            }
            out << '\n'; return 0;
        }
        [[= "Report rendering and framebuffer delivery diagnostics."_fs, = Name{"display status"_fs}]]
        auto status(ui::gui::Controller& gui, const Arguments&, std::ostream& out) -> CallbackResult {
            const auto s = gui.status();
            constexpr const char* names[]{"starting","running","unavailable","fault","stopped"};
            out << "GUI " << names[unsigned(s.state)] << "; rendered=" << s.frames << "; presented=" << s.presented
                << "; buffer=" << s.activeBuffer << "; max render=" << s.maximumRenderUs << " us; late=" << s.missedDeadlines
                << "; underruns=" << s.underruns << "; errors=" << s.errors << "; queue overflow=" << s.queueOverflow
                << "; graphics verified=" << s.graphicsVerified << "; interactions=" << s.interactions << "; value=" << s.value << "; animating=" << s.animating << '\n';
            out << "DMA errors=" << s.dmaErrors << "; render failures=" << s.renderFailures << "; input overflow=" << s.inputOverflow << '\n';
            return 0;
        }
    }
    void setup(Parser& parser, ui::gui::Controller& gui) {
        std::call_once(parser.m_displayRegistration, [&] {
            register_commands<^^commands>(parser, [&gui](auto command) {
                return [&gui, command](const Arguments& args, std::ostream& out) { return command(gui, args, out); };
            });
        });
    }
}
