#include "Controller.hpp"
#include "frontend/Frontend.hpp"
#include "hal/core/core.hpp"
#include "integration/GraphicsDMA.hpp"
#include "integration/OS.hpp"
#include "pneumo/logging.hpp"
#include "profiling/Trace.hpp"
#include "util/DirtyRegions.hpp"
#include "util/FrameTimer.hpp"
#include <algorithm>
#include <fonts/ApplicationFontProvider.hpp>
#include <images/BitmapDatabase.hpp>
#include <optional>
#include <platform/driver/lcd/LCD16bpp.hpp>
#include <texts/TextKeysAndLanguages.hpp>
#include <thread>
#include <touchgfx/Texts.hpp>
#include <touchgfx/hal/HAL.hpp>
#include <utility>

extern "C" void touchgfx_paint_link();
namespace ui::gui
{
    using namespace std::chrono_literals;
    namespace
    {
        constexpr unsigned DISPLAY_WIDTH{ 1024 }, DISPLAY_HEIGHT{ 600 }, DIAGNOSTIC_FRAMES{ 10 },
          BRIGHTNESS_FIELD{ 11 }, STARTUP_BRIGHTNESS{ 10 };
#if defined(CORE_CM7)
        constexpr unsigned STORAGE_ALIGNMENT{ 32 };
#endif
        constexpr unsigned DISPLAY_DEADLINE_US{ 16680 }, PERCENT{ 100 };
        constexpr std::array SUPPORTED_FRAME_RATES{ 1U, 2U, 5U, 10U };
        constexpr std::array BENCHMARK_TRIANGLES{ 500U, 2000U, 5000U };
        constexpr auto INPUT_FRESHNESS{ 100ms }, PRESENT_TIMEOUT{ 100ms }, FRAME_TIMEOUT{ 250ms },
          TOUCH_ACTIVE_PERIOD{ 8ms }, TOUCH_RETRY_PERIOD{ 1000ms };
    }
    // Member order encodes construction/destruction dependencies; 32 padding bytes are intentional.
    // NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
    struct Controller::Engine
    {
        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;
        Engine(Engine&&) = delete;
        Engine& operator=(Engine&&) = delete;
        struct Touch final : touchgfx::TouchController
        {
            Touch(const Touch&) = delete;
            Touch& operator=(const Touch&) = delete;
            Touch(Touch&&) = delete;
            Touch& operator=(Touch&&) = delete;
            ~Touch() override = default;
            Controller& owner;
            bool cancelled{};
            explicit Touch(Controller& controller)
              : owner{ controller }
            {
            }
            void init() override {}
            // TouchGFX virtual ABI fixes these coordinate output parameters.
            // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
            bool sampleTouch(std::int32_t& x, std::int32_t& y) override
            {
                Status current{};
                hal::device::TouchReport report{};
                {
                    std::scoped_lock lock{ owner.m_mutex };
                    current = owner.m_status;
                    report = owner.m_input;
                    owner.m_wheel.consume(report, std::chrono::steady_clock::now());
                }
                bool invalid{ current.touch_cancelled || !current.touch_available ||
                              std::chrono::steady_clock::now() - current.touch_time >= INPUT_FRESHNESS };
                invalid = owner.m_engine->frontend.input(report, !invalid) || invalid;
                if (invalid && !cancelled) {
                    touchgfx::Application::getInstance()->handleClickEvent(
                      touchgfx::ClickEvent(touchgfx::ClickEvent::CANCEL, 0, 0));
                }
                cancelled = invalid;
                x = current.touch_x;
                y = current.touch_y;
                return !invalid && current.touch_available && current.touch_down &&
                       std::chrono::steady_clock::now() - current.touch_time < INPUT_FRESHNESS;
            }
        };
        struct Port final : touchgfx::HAL
        {
            Port(const Port&) = delete;
            Port& operator=(const Port&) = delete;
            Port(Port&&) = delete;
            Port& operator=(Port&&) = delete;
            ~Port() override = default;
            Controller& owner;
            std::stop_token stop;
            util::FrameTimer timer{ hal::core::create_runtime_timer() };
            Port(Controller& controller,
                 touchgfx::DMA_Interface& dma,
                 touchgfx::LCD& lcd,
                 Touch& touch,
                 std::stop_token token)
              : HAL{ dma, lcd, touch, DISPLAY_WIDTH, DISPLAY_HEIGHT }
              , owner{ controller }
              , stop{ std::move(token) }
            {
            }
            std::uint16_t* getTFTFrameBuffer() const override
            {
                return owner.m_display->framebuffer(owner.m_display->state().active).data();
            }
            void setTFTFrameBuffer(std::uint16_t* address) override
            {
                const unsigned slot{ address == owner.m_display->framebuffer(0).data() ? 0U : 1U };
                if (owner.m_display->state().closed) {
                    return;
                }
                if (stop.stop_requested() || !owner.m_display->requestPresent(slot) ||
                    !owner.waitPresentation(slot, stop)) {
                    if (!owner.m_display->state().closed) {
                        owner.m_renderFailed = true;
                    }
                }
            }
            void configureInterrupts() override {} // display driver is the sole IRQ owner
            void enableInterrupts() override {}
            void disableInterrupts() override {}
            void enableLCDControllerInterrupt() override {}
            util::DirtyRegions dirty;
            void flushFrameBuffer(const touchgfx::Rect& region) override
            {
                HAL::flushFrameBuffer(region);
                dirty.add(region);
            }
            bool beginFrame() override
            {
                timer.start();
                return HAL::beginFrame();
            }
            void endFrame() override
            {
                HAL::endFrame();
                auto micros{ timer.elapsed().count() };
                const auto draw_us{ micros };
                std::uint32_t synchronize_us{};
                auto& graphics{ owner.m_engine->dma };
                if (graphics.failed()) {
                    owner.m_renderFailed = true;
                }
                const auto previous{ owner.m_display->state().active };
                if (!owner.m_renderFailed) {
                    swapFrameBuffers();
                }
                const auto active{ owner.m_display->state().active };
                if (!owner.m_renderFailed && active != previous) {
                    owner.m_scene.presented();
                    // Reload has completed: the former scanout buffer is now
                    // exclusively ours. Keep only changed regions synchronized.
                    timer.start();
                    profiling::Scope synchronization{ PROFILE_SITE_GUI_SYNC, PROFILE_CAT_GRAPHICS };
                    for (const auto& region : dirty.regions()) {
                        if (owner.m_renderFailed) {
                            break;
                        }
                        owner.m_renderFailed = !graphics.copyRegion(owner.m_display->framebuffer(active),
                                                                    owner.m_display->framebuffer(previous),
                                                                    region);
                    }
                    synchronize_us = timer.elapsed().count();
                    micros += synchronize_us;
                    dirty.clear();
                }
                std::scoped_lock lock{ owner.m_mutex };
                ++owner.m_status.frames;
                owner.m_status.draw_us = draw_us;
                owner.m_status.synchronize_us = synchronize_us;
                owner.m_status.total_draw_us += draw_us;
                owner.m_status.total_synchronize_us += synchronize_us;
                owner.m_status.image_transfer_us = graphics.imageTransferUs();
                owner.m_status.image_transfers = graphics.imageTransfers();
                owner.m_status.maximum_render_us =
                  std::max(owner.m_status.maximum_render_us, static_cast<std::uint32_t>(micros));
                if (std::cmp_greater(micros, DISPLAY_DEADLINE_US)) {
                    ++owner.m_status.missed_deadlines;
                }
            }
            void frame()
            {
                vSync();
                backPorchExited();
            }
        };
        os::Session runtime;
        GraphicsDMA dma;
        touchgfx::LCD16bpp lcd;
        Touch touch;
        Port port;
        ApplicationFontProvider fonts;
        touchgfx::Texts texts;
        struct Assets
        {
            Assets(touchgfx::Texts& texts, ApplicationFontProvider& fonts)
            {
                touchgfx::TypedText::registerTexts(&texts);
                touchgfx::Texts::setLanguage(EN);
                touchgfx::FontManager::setFontProvider(&fonts);
                touchgfx::Bitmap::registerBitmapDatabase(BitmapDatabase::getInstance(),
                                                         BitmapDatabase::getInstanceSize());
            }
        } assets;
        Frontend frontend;
        Engine(Controller& owner, const std::stop_token& stop)
          : runtime{ stop }
          , dma{ hal::core::create_graphics(), stop }
          , touch{ owner }
          , port{ owner, dma, lcd, touch, stop }
          , assets{ texts, fonts }
          , frontend{ owner.m_scene, owner.m_backend }
        {
            touchgfx_paint_link();
            port.initialize();
            port.setFrameBufferStartAddresses(
              owner.m_display->framebuffer(0).data(), owner.m_display->framebuffer(1).data(), nullptr);
            port.registerEventListener(frontend);
            const bool verified{ dma.verify(owner.m_display->framebuffer(1)) };
            {
                std::scoped_lock lock{ owner.m_mutex };
                owner.m_status.graphics_verified = verified;
            }
            if (!verified) {
                throw std::runtime_error("graphics pixel self-test failed");
            }
            frontend.setCommands([](void* pointer) {
                auto& controller{ *static_cast<Controller*>(pointer) };
                controller.commands(*controller.m_engine);
            }, &owner);
            frontend.changeToStartScreen();
        }
        ~Engine() = default;
    };
    void Controller::DeleteEngine::operator()(Engine* engine) const
    {
#if defined(CORE_CM7)
        std::destroy_at(engine);
#else
        std::default_delete<Engine>{}(engine);
#endif
    }
    Controller::Controller() = default;
    Controller::~Controller() = default;
    Status Controller::status() const
    {
        std::scoped_lock lock{ m_mutex };
        return m_status;
    }
    bool Controller::enqueue(Request request)
    {
        std::scoped_lock lock{ m_mutex };
        if (m_status.state == State::Stopped || m_status.state == State::Fault ||
            m_status.state == State::Unavailable) {
            return false;
        }
        if (m_count == m_requests.size()) {
            ++m_status.queue_overflow;
            return false;
        }
        m_requests[(m_head + m_count++) % m_requests.size()] = request;
        m_notification.signal();
        return true;
    }
    bool Controller::robotRate(unsigned fps, bool interactive)
    {
        return std::ranges::contains(SUPPORTED_FRAME_RATES, fps) &&
               enqueue({ .command = Command::Rate, .value = fps, .interactive = interactive });
    }
    bool Controller::bars() { return enqueue({ .command = Command::Bars, .value = 0 }); }
    bool Controller::showcase() { return enqueue({ .command = Command::Showcase, .value = 0 }); }
    bool Controller::scene(unsigned triangles)
    {
        return std::ranges::contains(BENCHMARK_TRIANGLES, triangles) &&
               enqueue({ .command = Command::Scene, .value = triangles });
    }
    bool Controller::backlight(unsigned percent)
    {
        return percent <= PERCENT && enqueue({ .command = Command::Backlight, .value = percent });
    }
    void Controller::commands(Engine& engine)
    {
        for (unsigned i{ 0 }; i < m_requests.size(); ++i) {
            Request request{};
            {
                std::scoped_lock lock{ m_mutex };
                if (!m_count) {
                    break;
                }
                request = m_requests[m_head];
                m_head = (m_head + 1) % m_requests.size();
                --m_count;
            }
            if (request.command == Command::Bars) {
                engine.frontend.diagnostics(true);
            }
            else if (request.command == Command::Showcase) {
                engine.frontend.diagnostics(false);
            }
            else if (request.command == Command::Scene) {
                engine.frontend.scene(request.value);
            }
            else if (request.command == Command::Rate) {
                engine.frontend.rate(request.value, request.interactive);
            }
            else if (!m_display->backlight(request.value)) {
                m_renderFailed = true;
            }
        }
    }
    bool Controller::waitPresentation(unsigned slot, const std::stop_token& stop)
    {
        profiling::Scope measurement{ PROFILE_SITE_PRESENT, PROFILE_CAT_GRAPHICS };
        const auto deadline{ std::chrono::steady_clock::now() + PRESENT_TIMEOUT };
        for (;;) {
            m_display->service();
            const auto state{ m_display->state() };
            if (!state.pending) {
                return state.active == slot && !state.errors;
            }
            if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            m_notification.waitUntil(
              std::min(deadline, std::chrono::steady_clock::now() + m_display->servicePeriod()));
        }
    }
    void Controller::renderFrame(hal::display::State state, bool& illuminated)
    {
        if (m_engine->frontend.model().ticks % DIAGNOSTIC_FRAMES == 0) {
            const auto rendering{ m_scene.status() };
            m_engine->frontend.graphics({ state.presentations,
                                          state.underruns,
                                          m_engine->dma.errors(),
                                          rendering.presented,
                                          rendering.last_render_us,
                                          rendering.replaced,
                                          rendering.last_latency_us,
                                          rendering.maximum_render_us });
        }
        m_engine->port.frame();
        if (m_engine->frontend.brightnessChanged()) {
            static_cast<void>(m_display->backlight(
              static_cast<unsigned>(m_engine->frontend.model().values[BRIGHTNESS_FIELD])));
        }
        state = m_display->state();
        if (!illuminated && state.presentations && !m_renderFailed) {
            static_cast<void>(m_display->backlight(STARTUP_BRIGHTNESS));
            illuminated = true;
        }
        std::scoped_lock lock{ m_mutex };
        m_status.presented = state.presentations;
        m_status.active_buffer = state.active;
        m_status.underruns = state.underruns;
        m_status.errors = state.errors;
        m_status.dma_errors = m_engine->dma.errors();
        m_status.interactions = m_engine->frontend.model().interactions;
        m_status.value = m_engine->frontend.model().value;
        m_status.animating = m_engine->frontend.model().animating;
    }
    bool Controller::run(const std::stop_token& stop)
    {
        const std::stop_callback cancellation{ stop, [this] { m_notification.signal(); } };
        m_display = hal::core::create_display();
        if (!m_display || !m_display->initialize()) {
            {
                std::scoped_lock lock{ m_mutex };
                m_status.state = State::Unavailable;
            }
            pnm::log::warn("GUI display unavailable; CLI and ICC remain active");
            return false;
        }
        m_display->setCallback({ .function = [](void* pointer, hal::display::Event) noexcept {
            static_cast<Controller*>(pointer)->m_notification.signal();
        }, .context = this });
        bool closed{};
        try {
#if defined(CORE_CM7)
            // CPU-only widgets/port metadata. No DMA reads this storage: pixels
            // remain in SDRAM. Linker reserves it separately from ITCM code.
            alignas(STORAGE_ALIGNMENT)
              __attribute__((section(".gui_internal"))) static std::array<std::byte, sizeof(Engine)>
                storage{};
            // Placement construction in linker-reserved, correctly aligned internal RAM.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            m_engine.reset(std::construct_at(reinterpret_cast<Engine*>(storage.data()), *this, stop));
#else
            m_engine = std::unique_ptr<Engine, DeleteEngine>{ new Engine{ *this, stop } };
#endif
            {
                std::scoped_lock lock{ m_mutex };
                m_status.state = State::Running;
            }
            bool illuminated{};
            std::uint32_t last_sync{ m_display->state().vsyncs };
            auto last_frame{ std::chrono::steady_clock::now() };
            while (!stop.stop_requested() && !m_renderFailed) {
                m_display->service();
                auto state{ m_display->state() };
                if (state.closed) {
                    closed = true;
                    break;
                }
                if (state.errors) {
                    m_renderFailed = true;
                    break;
                }
                if (state.vsyncs != last_sync) {
                    last_sync = state.vsyncs;
                    last_frame = std::chrono::steady_clock::now();
                    renderFrame(state, illuminated);
                }
                else if (std::chrono::steady_clock::now() - last_frame > FRAME_TIMEOUT) {
                    m_renderFailed = true;
                    break;
                }
                // Presentation/DMA waits can consume a notification carrying
                // the next VSync. Recheck the counter before sleeping so that
                // coalescing never silently skips an already available frame.
                if (m_display->state().vsyncs == last_sync) {
                    m_notification.waitUntil(std::chrono::steady_clock::now() + m_display->servicePeriod());
                }
            }
        } catch (const std::exception& error) {
            pnm::log::error("GUI rendering failed: {}", error.what());
            m_renderFailed = true;
        }
        {
            std::scoped_lock lock{ m_mutex };
            const auto final_state{ m_display->state() };
            m_status.underruns = final_state.underruns;
            m_status.errors = final_state.errors;
            if (m_engine) {
                m_status.dma_errors = m_engine->dma.errors();
            }
            if (m_renderFailed && !stop.stop_requested()) {
                ++m_status.render_failures;
            }
        }
        if (m_backend) {
            m_backend->release();
        }
        m_display->setCallback({});
        m_display->shutdown(); // aborts pending ownership before engine/buffers disappear
        m_scene.stopAndWait();
        m_engine.reset();
        m_display.reset();
        {
            std::scoped_lock lock{ m_mutex };
            m_status.state = m_renderFailed && !stop.stop_requested() ? State::Fault : State::Stopped;
        }
        return closed;
    }
    Controller::TouchSample Controller::selectTouch(const hal::util::Result<hal::device::TouchReport>& report,
                                                    TouchSelection& selection)
    {
        hal::device::TouchPoint point{};
        if (report) {
            if (selection.primary && !report->points[*selection.primary].active) {
                selection.primary.reset();
            }
            else if (!selection.primary) {
                for (unsigned i{ 0 }; i < report->points.size(); ++i) {
                    if (report->points[i].active) {
                        selection.primary = i;
                        break;
                    }
                }
            }
            if (selection.primary) {
                point = report->points[*selection.primary];
            }
            // Panel coordinates are landscape/native; reject, never wrap,
            // out-of-range data. Rotation belongs here if hardware changes.
            if (point.x >= DISPLAY_WIDTH || point.y >= DISPLAY_HEIGHT) {
                point.active = false;
            }
        }
        else {
            selection.primary.reset();
        }
        unsigned contacts{};
        if (report) {
            for (const auto& p : report->points) {
                contacts += p.active;
            }
        }
        if (contacts > 1) {
            selection.multiple = true;
        }
        if (!contacts) {
            selection.multiple = false;
        }
        return { .point = point, .contacts = contacts };
    }
    void Controller::publishTouch(const hal::util::Result<hal::device::TouchReport>& report,
                                  hal::device::TouchPoint point,
                                  bool multiple)
    {
        bool input_cancelled{};
        {
            std::scoped_lock lock{ m_mutex };
            m_input = report ? *report : hal::device::TouchReport{ .cancelled = true };
            if (!m_wheel.push(m_input, std::chrono::steady_clock::now())) {
                m_input.cancelled = true;
            }
            m_status.touch_available = bool(report);
            m_status.touch_down = point.active;
            m_status.touch_cancelled = input_cancelled = m_input.cancelled;
            if (report) {
                m_status.input_overflow = report->dropped_reports + m_wheel.dropped();
            }
            m_status.touch_x = point.x;
            m_status.touch_y = point.y;
            m_status.touch_time = std::chrono::steady_clock::now();
        }
        if (m_backend) {
            m_backend->input({ .down = point.active && !multiple,
                               .valid = bool(report) && !input_cancelled && !multiple,
                               .x = int(point.x),
                               .y = int(point.y),
                               .sampled = std::chrono::steady_clock::now() });
        }
    }
    void Controller::acquireTouch(const std::stop_token& stop)
    {
        const std::stop_callback cancellation{ stop, [this] { m_touchNotification.signal(); } };
        auto touch{ hal::core::create_touch() };
        if (!touch) {
            return;
        }
        touch->setCallback([](void* pointer) noexcept {
            static_cast<Controller*>(pointer)->m_touchNotification.signal();
        }, this);
        bool ready{};
        TouchSelection selection{};
        while (!stop.stop_requested()) {
            if (!ready) {
                ready = bool(touch->initialize());
            }
            auto report{ ready ? touch->read()
                               : hal::util::Result<hal::device::TouchReport>{
                                   std::unexpected(std::make_error_code(std::errc::no_such_device)) } };
            const auto sample{ selectTouch(report, selection) };
            if (!report) {
                ready = false;
            }
            publishTouch(report, sample.point, selection.multiple);
            auto period{ TOUCH_RETRY_PERIOD };
            if (ready) {
                period = sample.contacts ? TOUCH_ACTIVE_PERIOD : touch->idlePeriod();
            }
            m_touchNotification.waitUntil(std::chrono::steady_clock::now() + period);
        }
        touch->setCallback(nullptr, nullptr);
        {
            std::scoped_lock lock{ m_mutex };
            m_status.touch_available = false;
            m_status.touch_down = false;
        }
    }
}
