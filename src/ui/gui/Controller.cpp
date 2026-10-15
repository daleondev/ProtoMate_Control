#include "Controller.hpp"
#include "frontend/Frontend.hpp"
#include "hal/core/core.hpp"
#include "integration/GraphicsDMA.hpp"
#include "integration/OS.hpp"
#include "pneumo/logging.hpp"
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

extern "C" void touchgfx_paint_link();
namespace ui::gui
{
    using namespace std::chrono_literals;
    struct Controller::Engine
    {
        struct Touch final : touchgfx::TouchController
        {
            Controller& owner;
            bool cancelled{};
            explicit Touch(Controller& controller)
              : owner(controller)
            {
            }
            void init() override {}
            bool sampleTouch(std::int32_t& x, std::int32_t& y) override
            {
                const auto current = owner.status();
                const bool invalid = current.touchCancelled || !current.touchAvailable ||
                                     std::chrono::steady_clock::now() - current.touchTime >= 100ms;
                if (invalid && !cancelled) {
                    touchgfx::Application::getInstance()->handleClickEvent(
                      touchgfx::ClickEvent(touchgfx::ClickEvent::CANCEL, 0, 0));
                }
                cancelled = invalid;
                x = current.touchX;
                y = current.touchY;
                return current.touchAvailable && current.touchDown &&
                       std::chrono::steady_clock::now() - current.touchTime < 100ms;
            }
        };
        struct Port final : touchgfx::HAL
        {
            Controller& owner;
            std::stop_token stop;
            util::FrameTimer timer{ hal::core::create_runtime_timer() };
            Port(Controller& controller,
                 touchgfx::DMA_Interface& dma,
                 touchgfx::LCD& lcd,
                 Touch& touch,
                 std::stop_token token)
              : HAL(dma, lcd, touch, 1024, 600)
              , owner(controller)
              , stop(token)
            {
            }
            std::uint16_t* getTFTFrameBuffer() const override
            {
                return owner.m_display->framebuffer(owner.m_display->state().active).data();
            }
            void setTFTFrameBuffer(std::uint16_t* address) override
            {
                const unsigned slot = address == owner.m_display->framebuffer(0).data() ? 0 : 1;
                if (stop.stop_requested() || !owner.m_display->requestPresent(slot) ||
                    !owner.waitPresentation(slot, stop)) {
                    owner.m_renderFailed = true;
                }
            }
            void configureInterrupts() override {} // display driver is the sole IRQ owner
            void enableInterrupts() override {}
            void disableInterrupts() override {}
            void enableLCDControllerInterrupt() override {}
            std::array<touchgfx::Rect, 16> dirty{};
            unsigned dirtyCount{};
            void flushFrameBuffer(const touchgfx::Rect& region) override
            {
                HAL::flushFrameBuffer(region);
                auto clipped = region;
                clipped.restrictTo(1024, 600);
                if (clipped.isEmpty()) {
                    return;
                }
                if (dirtyCount < dirty.size()) {
                    dirty[dirtyCount++] = clipped;
                }
                else {
                    dirty[0] = { 0, 0, 1024, 600 };
                    dirtyCount = 1;
                }
            }
            bool beginFrame() override
            {
                timer.start();
                return HAL::beginFrame();
            }
            void endFrame() override
            {
                HAL::endFrame();
                auto micros = timer.elapsed().count();
                auto& graphics = static_cast<GraphicsDMA&>(dma);
                if (graphics.failed()) {
                    owner.m_renderFailed = true;
                }
                const auto previous = owner.m_display->state().active;
                if (!owner.m_renderFailed) {
                    swapFrameBuffers();
                }
                const auto active = owner.m_display->state().active;
                if (!owner.m_renderFailed && active != previous) {
                    // Reload has completed: the former scanout buffer is now
                    // exclusively ours. Keep only changed regions synchronized.
                    timer.start();
                    for (unsigned i = 0; i < dirtyCount && !owner.m_renderFailed; ++i) {
                        owner.m_renderFailed = !graphics.copyRegion(owner.m_display->framebuffer(active),
                                                                    owner.m_display->framebuffer(previous),
                                                                    dirty[i]);
                    }
                    micros += timer.elapsed().count();
                    dirtyCount = 0;
                }
                std::scoped_lock lock(owner.m_mutex);
                ++owner.m_status.frames;
                owner.m_status.maximumRenderUs =
                  std::max(owner.m_status.maximumRenderUs, static_cast<std::uint32_t>(micros));
                if (micros > 16669) {
                    ++owner.m_status.missedDeadlines;
                }
            }
            void frame()
            {
                vSync();
                backPorchExited();
            }
        };
        GraphicsDMA dma;
        touchgfx::LCD16bpp lcd;
        Touch touch;
        Port port;
        ApplicationFontProvider fonts;
        touchgfx::Texts texts;
        struct Assets
        {
            Assets()
            {
                touchgfx::Bitmap::registerBitmapDatabase(BitmapDatabase::getInstance(),
                                                         BitmapDatabase::getInstanceSize());
            }
        } assets;
        Frontend frontend;
        Engine(Controller& owner, std::stop_token stop)
          : dma(hal::core::create_graphics(), stop)
          , touch(owner)
          , port(owner, dma, lcd, touch, stop)
        {
            touchgfx_paint_link();
            os::prepare(stop);
            touchgfx::TypedText::registerTexts(&texts);
            touchgfx::Texts::setLanguage(EN);
            touchgfx::FontManager::setFontProvider(&fonts);
            port.initialize();
            port.setFrameBufferStartAddresses(
              owner.m_display->framebuffer(0).data(), owner.m_display->framebuffer(1).data(), nullptr);
            port.registerEventListener(frontend);
            const bool verified = dma.verify(owner.m_display->framebuffer(1));
            {
                std::scoped_lock lock(owner.m_mutex);
                owner.m_status.graphicsVerified = verified;
            }
            if (!verified) {
                throw std::runtime_error("graphics pixel self-test failed");
            }
            frontend.setCommands([](void* pointer) {
                auto& controller = *static_cast<Controller*>(pointer);
                controller.commands(*controller.m_engine);
            }, &owner);
            frontend.changeToStartScreen();
        }
        ~Engine() { os::finish(); }
    };
    Controller::Controller() = default;
    Controller::~Controller() = default;
    Status Controller::status() const
    {
        std::scoped_lock lock(m_mutex);
        return m_status;
    }
    bool Controller::enqueue(Request request)
    {
        std::scoped_lock lock(m_mutex);
        if (m_status.state == State::Stopped || m_status.state == State::Fault ||
            m_status.state == State::Unavailable) {
            return false;
        }
        if (m_count == m_requests.size()) {
            ++m_status.queueOverflow;
            return false;
        }
        m_requests[(m_head + m_count++) % m_requests.size()] = request;
        m_notification.signal();
        return true;
    }
    bool Controller::bars() { return enqueue({ Command::Bars, 0 }); }
    bool Controller::showcase() { return enqueue({ Command::Showcase, 0 }); }
    bool Controller::backlight(unsigned percent)
    {
        return percent <= 100 && enqueue({ Command::Backlight, percent });
    }
    void Controller::commands(Engine& engine)
    {
        for (unsigned i = 0; i < m_requests.size(); ++i) {
            Request request{};
            {
                std::scoped_lock lock(m_mutex);
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
            else if (!m_display->backlight(request.value)) {
                m_renderFailed = true;
            }
        }
    }
    bool Controller::waitPresentation(unsigned slot, std::stop_token stop)
    {
        const auto deadline = std::chrono::steady_clock::now() + 100ms;
        for (;;) {
            m_display->service();
            const auto state = m_display->state();
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
    bool Controller::run(std::stop_token stop)
    {
        const std::stop_callback cancellation(stop, [this] { m_notification.signal(); });
        m_display = hal::core::create_display();
        if (!m_display || !m_display->initialize()) {
            {
                std::scoped_lock lock(m_mutex);
                m_status.state = State::Unavailable;
            }
            pnm::log::warn("GUI display unavailable; CLI and ICC remain active");
            return false;
        }
        m_display->setCallback({ [](void* pointer, hal::display::Event) noexcept {
            static_cast<Controller*>(pointer)->m_notification.signal();
        }, this });
        bool closed{};
        try {
            m_engine = std::make_unique<Engine>(*this, stop);
            {
                std::scoped_lock lock(m_mutex);
                m_status.state = State::Running;
            }
            bool illuminated{};
            std::uint32_t lastSync = m_display->state().vsyncs;
            auto lastFrame = std::chrono::steady_clock::now();
            while (!stop.stop_requested() && !m_renderFailed) {
                m_display->service();
                auto state = m_display->state();
                if (state.closed) {
                    closed = true;
                    break;
                }
                if (state.errors) {
                    m_renderFailed = true;
                    break;
                }
                if (state.vsyncs != lastSync) {
                    lastSync = state.vsyncs;
                    lastFrame = std::chrono::steady_clock::now();
                    m_engine->port.frame();
                    state = m_display->state();
                    if (!illuminated && state.presentations && !m_renderFailed) {
                        static_cast<void>(m_display->backlight(10));
                        illuminated = true;
                    }
                    std::scoped_lock lock(m_mutex);
                    m_status.presented = state.presentations;
                    m_status.activeBuffer = state.active;
                    m_status.underruns = state.underruns;
                    m_status.errors = state.errors;
                    m_status.dmaErrors = m_engine->dma.errors();
                    m_status.interactions = m_engine->frontend.model().interactions;
                    m_status.value = m_engine->frontend.model().value;
                    m_status.animating = m_engine->frontend.model().animating;
                }
                else if (std::chrono::steady_clock::now() - lastFrame > 250ms) {
                    m_renderFailed = true;
                    break;
                }
                // Presentation/DMA waits can consume a notification carrying
                // the next VSync. Recheck the counter before sleeping so that
                // coalescing never silently skips an already available frame.
                if (m_display->state().vsyncs == lastSync) {
                    m_notification.waitUntil(std::chrono::steady_clock::now() + m_display->servicePeriod());
                }
            }
        } catch (const std::exception& error) {
            pnm::log::error("GUI rendering failed: {}", error.what());
            m_renderFailed = true;
        }
        {
            std::scoped_lock lock(m_mutex);
            const auto finalState = m_display->state();
            m_status.underruns = finalState.underruns;
            m_status.errors = finalState.errors;
            if (m_engine) {
                m_status.dmaErrors = m_engine->dma.errors();
            }
            if (m_renderFailed && !stop.stop_requested()) {
                ++m_status.renderFailures;
            }
        }
        m_display->setCallback({});
        m_display->shutdown(); // aborts pending ownership before engine/buffers disappear
        m_engine.reset();
        os::finish();
        m_display.reset();
        {
            std::scoped_lock lock(m_mutex);
            m_status.state = m_renderFailed && !stop.stop_requested() ? State::Fault : State::Stopped;
        }
        return closed;
    }
    void Controller::acquireTouch(std::stop_token stop)
    {
        const std::stop_callback cancellation(stop, [this] { m_touchNotification.signal(); });
        auto touch = hal::core::create_touch();
        if (!touch) {
            return;
        }
        touch->setCallback([](void* pointer) noexcept {
            static_cast<Controller*>(pointer)->m_touchNotification.signal();
        }, this);
        bool ready{};
        std::optional<unsigned> primary;
        while (!stop.stop_requested()) {
            if (!ready) {
                ready = bool(touch->initialize());
            }
            auto report = ready ? touch->read()
                                : hal::util::Result<hal::device::TouchReport>{ std::unexpected(
                                    std::make_error_code(std::errc::no_such_device)) };
            hal::device::TouchPoint point;
            if (report) {
                if (primary && !report->points[*primary].active) {
                    primary.reset();
                }
                else if (!primary) {
                    for (unsigned i = 0; i < report->points.size(); ++i) {
                        if (report->points[i].active) {
                            primary = i;
                            break;
                        }
                    }
                }
                if (primary) {
                    point = report->points[*primary];
                }
                // Panel coordinates are landscape/native; reject, never wrap,
                // out-of-range data. Rotation belongs here if hardware changes.
                if (point.x >= 1024 || point.y >= 600) {
                    point.active = false;
                }
            }
            else {
                ready = false;
                primary.reset();
            }
            {
                std::scoped_lock lock(m_mutex);
                m_status.touchAvailable = bool(report);
                m_status.touchDown = point.active;
                m_status.touchCancelled = !report || report->cancelled;
                if (report) {
                    m_status.inputOverflow = report->droppedReports;
                }
                m_status.touchX = point.x;
                m_status.touchY = point.y;
                m_status.touchTime = std::chrono::steady_clock::now();
            }
            m_touchNotification.waitUntil(std::chrono::steady_clock::now() +
                                          (ready ? (point.active ? 8ms : touch->idlePeriod()) : 1000ms));
        }
        touch->setCallback(nullptr, nullptr);
        {
            std::scoped_lock lock(m_mutex);
            m_status.touchAvailable = false;
            m_status.touchDown = false;
        }
    }
}
