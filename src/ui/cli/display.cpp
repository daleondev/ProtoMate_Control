#include "display.hpp"
#include "Parser.hpp"
#include "ui/gui/Controller.hpp"
#include <array>
#include <charconv>
#include <ostream>
namespace ui::cli::display
{
    namespace commands
    {
        using namespace pnm::meta::string::literals;
        [[ = "Show RGB color bars through the GUI renderer."_fs,
           = Name{ "display bars"_fs } ]] auto bars(ui::gui::Controller& gui,
                                                    const Arguments& /*unused*/,
                                                    std::ostream& /*unused*/) -> CallbackResult
        {
            return gui.bars() ? CallbackResult{ 0 }
                              : callback_failure("GUI unavailable or request queue full");
        }
        [[ = "Return to the robot Home screen."_fs,
           = Name{ "display gui"_fs } ]] auto showcase(ui::gui::Controller& gui,
                                                       const Arguments& /*unused*/,
                                                       std::ostream& /*unused*/) -> CallbackResult
        {
            return gui.showcase() ? CallbackResult{ 0 }
                                  : callback_failure("GUI unavailable or request queue full");
        }
        [[
            = "Run the shaded 3D performance benchmark."_fs,
            = Name{ "display scene"_fs },
            = Arg{ .name = "triangles"_fs, .description = "500, 2000 or 5000"_fs }
        ]] auto scene(ui::gui::Controller& gui, const Arguments& args, std::ostream& /*unused*/)
          -> CallbackResult
        {
            unsigned triangles{};
            const auto text{ args.require("triangles") };
            const auto [end, error]{ std::from_chars(text.data(), text.data() + text.size(), triangles) };
            if (error != std::errc{} || end != text.data() + text.size() || !gui.scene(triangles)) {
                return callback_failure("Expected 500, 2000 or 5000; GUI must be available");
            }
            return 0;
        }
        [[
            = "Robot camera test (synthetic camera only, no motor commands)."_fs,
            = Name{ "display robot-test"_fs },
            = Arg{ .name = "fps"_fs, .description = "10, 5, 2 or 1"_fs },
            = Flag{ .name = "interactive"_fs,
                    .description = "Use the motion-detail mesh; default is full detail"_fs }
        ]] auto robot_test(ui::gui::Controller& gui, const Arguments& args, std::ostream& /*unused*/)
          -> CallbackResult
        {
            unsigned rate{};
            const auto text{ args.require("fps") };
            const auto [end, error]{ std::from_chars(text.data(), text.data() + text.size(), rate) };
            if (error != std::errc{} || end != text.data() + text.size() ||
                !gui.robotRate(rate, args.hasFlag("interactive"))) {
                return callback_failure("Expected 10, 5, 2 or 1; return to normal with display gui");
            }
            return 0;
        }
        [[
            = "Set display backlight (0..100 percent)."_fs,
            = Name{ "display backlight"_fs },
            = Arg{ .name = "percent"_fs, .description = "0..100"_fs }
        ]] auto backlight(ui::gui::Controller& gui, const Arguments& args, std::ostream& /*unused*/)
          -> CallbackResult
        {
            constexpr unsigned max_percent{ 100 };
            unsigned percent{};
            const auto text{ args.require("percent") };
            const auto [end, error]{ std::from_chars(text.data(), text.data() + text.size(), percent) };
            if (error != std::errc{} || end != text.data() + text.size() || percent > max_percent) {
                return callback_failure("Expected 0..100");
            }
            return gui.backlight(percent) ? CallbackResult{ 0 }
                                          : callback_failure("GUI unavailable or request queue full");
        }
        [[
            = "Report cached touch state; performs no I2C transaction."_fs,
            = Name{ "display touch"_fs }
        ]] auto touch(ui::gui::Controller& gui, const Arguments& /*unused*/, std::ostream& out)
          -> CallbackResult
        {
            const auto status{ gui.status() };
            out << "Touch " << (status.touch_available ? "available" : "unavailable")
                << "; contact=" << status.touch_down << "; x=" << status.touch_x << "; y=" << status.touch_y;
            if (status.touch_time != std::chrono::steady_clock::time_point{}) {
                out << "; age="
                    << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - status.touch_time)
                         .count()
                    << " ms";
            }
            out << '\n';
            return 0;
        }
        [[
            = "Report rendering and framebuffer delivery diagnostics."_fs,
            = Name{ "display status"_fs }
        ]] auto status(ui::gui::Controller& gui, const Arguments& /*unused*/, std::ostream& out)
          -> CallbackResult
        {
            const auto s{ gui.status() };
            constexpr std::array names{ "starting", "running", "unavailable", "fault", "stopped" };
            out << "GUI " << names[unsigned(s.state)] << "; rendered=" << s.frames
                << "; presented=" << s.presented << "; buffer=" << s.active_buffer
                << "; max render=" << s.maximum_render_us << " us; late=" << s.missed_deadlines
                << "; underruns=" << s.underruns << "; errors=" << s.errors
                << "; queue overflow=" << s.queue_overflow << "; graphics verified=" << s.graphics_verified
                << "; interactions=" << s.interactions << "; value=" << s.value
                << "; animating=" << s.animating << '\n';
            out << "DMA errors=" << s.dma_errors << "; render failures=" << s.render_failures
                << "; input overflow=" << s.input_overflow << '\n';
            out << "GUI draw us=" << s.draw_us << "; synchronization us=" << s.synchronize_us << '\n';
            out << "GUI total draw us=" << s.total_draw_us << "; total sync us=" << s.total_synchronize_us
                << "; image transfer us=" << s.image_transfer_us << "; image transfers=" << s.image_transfers
                << '\n';
            const auto r{ gui.sceneStatus() };
            out << "3D running=" << r.running << "; visible=" << r.visible << "; failed=" << r.failed
                << "; rendered=" << r.rendered << "; acquired=" << r.acquired << "; presented=" << r.presented
                << "; presented revision=" << r.presented_revision << "; latency us=" << r.last_latency_us
                << "; max latency us=" << r.maximum_latency_us << "; last us=" << r.last_render_us
                << "; max us=" << r.maximum_render_us << "; clear us=" << r.clear_us
                << "; geometry us=" << r.geometry_us << "; total us=" << r.total_render_us
                << "; completed revision=" << r.completed_revision
                << "; acquired revision=" << r.acquired_revision << "; replaced=" << r.replaced
                << "; rejected=" << r.rejected << '\n';
            out << "3D grid us=" << r.grid_us << "; base us=" << r.node_us[0]
                << "; shoulder us=" << r.node_us[1] << "; elbow us=" << r.node_us[2]
                << "; tool us=" << r.node_us[3] << '\n';
            out << "3D completed triangles=" << r.completed_triangles
                << "; acquired triangles=" << r.acquired_triangles << '\n';
            return 0;
        }
    }
    void setup(Parser& parser, ui::gui::Controller& gui)
    {
        parser.registerModule(Parser::Module::Display, [&] {
            register_commands<^^commands>(parser, [&gui](auto command) {
                return [&gui, command](const Arguments& args, std::ostream& out) {
                    return command(gui, args, out);
                };
            });
        });
    }
}
