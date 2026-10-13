#include "display.hpp"
#include "Parser.hpp"
#include "hal/core/core.hpp"
#include <array>
#include <charconv>
#include <memory>
#include <ostream>
namespace ui::cli::display {
    namespace {
        struct Context {
            std::shared_ptr<hal::device::IDisplay> panel;
            std::shared_ptr<hal::device::ITouch> touch;
            unsigned slot{};
            auto ensurePanel() -> CallbackResult
            {
                if (panel) { return 0; }
                auto next = hal::core::create_display();
                if (!next) { return callback_failure("RGB display is unavailable on this platform"); }
                const auto result = next->initialize();
                if (!result) { return callback_failure(result.error().message()); }
                panel = std::move(next);
                return 0;
            }
        };
    }
    namespace commands {
        using namespace pnm::meta::string::literals;
        [[= "Show RGB565 color bars; call twice to test both framebuffers. Backlight starts at 10%."_fs, = Name{"display bars"_fs}]]
        auto bars(Context& context, const Arguments&, std::ostream& out) -> CallbackResult
        {
            auto ready = context.ensurePanel();
            if (!ready) { return ready; }
            context.slot ^= 1;
            // Exercise DMA2D before the CPU pattern; inactive framebuffer only.
            auto result = context.panel->fill(context.slot, 0);
            if (!result) { return callback_failure(result.error().message()); }
            constexpr std::array<std::uint16_t,8> colors{0xFFFF,0xFFE0,0x07FF,0x07E0,0xF81F,0xF800,0x001F,0};
            const auto pixels = context.panel->framebuffer(context.slot);
            const auto width = context.panel->width();
            for (unsigned y = 0; y < context.panel->height(); ++y) {
                for (unsigned x = 0; x < width; ++x) { pixels[y * width + x] = colors[x * colors.size() / width]; }
            }
            result = context.panel->present(context.slot);
            if (result) { result = context.panel->backlight(10); }
            if (!result) { return callback_failure(result.error().message()); }
            out << "Framebuffer " << context.slot << "; LTDC underruns=" << context.panel->underruns() << '\n';
            return 0;
        }
        [[= "Set backlight duty (0..100 percent); requires external display power."_fs, = Name{"display backlight"_fs},
          = Arg{.name="percent"_fs, .description="0..100"_fs}]]
        auto backlight(Context& context, const Arguments& args, std::ostream&) -> CallbackResult
        {
            unsigned percent{};
            const auto text = args.require("percent");
            const auto [end, error] = std::from_chars(text.data(), text.data()+text.size(), percent);
            if (error != std::errc{} || end != text.data()+text.size() || percent > 100) { return callback_failure("Expected 0..100"); }
            const auto ready = context.ensurePanel();
            if (!ready) { return ready; }
            const auto result = context.panel->backlight(percent);
            return result ? CallbackResult{0} : callback_failure(result.error().message());
        }
        [[= "Read ST1633i firmware/status and up to five touch coordinates."_fs, = Name{"display touch"_fs}]]
        auto touch(Context& context, const Arguments&, std::ostream& out) -> CallbackResult
        {
            if (!context.touch) {
                auto next = hal::core::create_touch();
                if (!next) { return callback_failure("Touch controller is unavailable on this platform"); }
                const auto result = next->initialize();
                if (!result) { return callback_failure("Touch absent or not ready: " + result.error().message()); }
                context.touch = std::move(next);
            }
            const auto report = context.touch->read();
            if (!report) { return callback_failure(report.error().message()); }
            out << "ST1633i firmware=" << unsigned(report->firmware) << " status=" << unsigned(report->status) << '\n';
            for (unsigned i = 0; i < report->points.size(); ++i) {
                const auto& point = report->points[i];
                if (point.active) { out << i << ": " << point.x << ", " << point.y << '\n'; }
            }
            return 0;
        }
    }
    void setup(Parser& parser)
    {
        std::call_once(parser.m_displayRegistration, [&parser] {
            auto context = std::make_shared<Context>();
            register_commands<^^commands>(parser, [context](auto command) {
                return [context, command](const Arguments& args, std::ostream& out) {
                    return command(*context, args, out);
                };
            });
        });
    }
}
