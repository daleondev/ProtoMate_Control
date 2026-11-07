#include "hal/drivers/impl/linux/Timer.hpp"
#include "runtime/thread.hpp"
#include "ui/gui/integration/GraphicsDMA.hpp"
#include "ui/gui/integration/OS.hpp"
#include "ui/gui/util/DirtyRegions.hpp"
#include "ui/gui/util/FrameTimer.hpp"
#include <array>
#include <ctime>
#include <gtest/gtest.h>
#include <random>
#include <thread>
#include <touchgfx/Bitmap.hpp>
namespace
{
    class GuiGraphics : public ::testing::Test
    {
        void SetUp() override { ui::gui::os::prepare({}); }
        void TearDown() override { ui::gui::os::finish(); }
    };
    class Graphics final : public hal::IGraphics
    {
      public:
        enum class Mode
        {
            Inline,
            Error,
            Lost
        } mode{ Mode::Inline };
        bool pending{};
        unsigned aborted{};
        hal::graphics::Callback callback{};
        auto submit(const hal::graphics::Transfer&, hal::graphics::Callback next)
          -> hal::util::Result<> override
        {
            callback = next;
            pending = true;
            if (mode != Mode::Lost) {
                pending = false;
                callback(mode == Mode::Error ? hal::graphics::Completion::Error
                                             : hal::graphics::Completion::Complete);
                callback = {};
            }
            return {};
        }
        void abort() noexcept override
        {
            ++aborted;
            pending = false;
            const auto old = callback;
            callback = {};
            old(hal::graphics::Completion::Aborted);
        }
        bool busy() const noexcept override { return pending; }
        std::uint32_t errors() const noexcept override { return mode == Mode::Error ? 1 : 0; }
    };
    touchgfx::BlitOp operation(std::span<std::uint16_t> pixels)
    {
        touchgfx::BlitOp op{};
        op.operation = touchgfx::BLIT_OP_FILL;
        op.pDst = pixels.data();
        op.nSteps = 4;
        op.nLoops = 4;
        op.dstLoopStride = 4;
        op.dstFormat = touchgfx::Bitmap::RGB565;
        op.alpha = 255;
        op.color = 0xffff;
        return op;
    }
}
TEST_F(GuiGraphics, InlineCompletionIsNotLost)
{
    auto hardware = std::make_shared<Graphics>();
    std::array<std::uint16_t, 16> pixels{};
    ui::gui::GraphicsDMA graphics(hardware, {});
    graphics.addToQueue(operation(pixels));
    EXPECT_FALSE(graphics.failed());
    EXPECT_FALSE(hardware->busy());
}
TEST_F(GuiGraphics, ErrorPreventsPresentationAndClearsCallback)
{
    auto hardware = std::make_shared<Graphics>();
    hardware->mode = Graphics::Mode::Error;
    std::array<std::uint16_t, 16> pixels{};
    ui::gui::GraphicsDMA graphics(hardware, {});
    graphics.addToQueue(operation(pixels));
    EXPECT_TRUE(graphics.failed());
    EXPECT_FALSE(hardware->callback.function);
}
TEST_F(GuiGraphics, LostCompletionTimesOutAndAbortsBeforeBufferRelease)
{
    auto hardware = std::make_shared<Graphics>();
    hardware->mode = Graphics::Mode::Lost;
    std::array<std::uint16_t, 16> pixels{};
    ui::gui::GraphicsDMA graphics(hardware, {});
    const auto start = std::chrono::steady_clock::now();
    graphics.addToQueue(operation(pixels));
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds{ 100 });
    EXPECT_TRUE(graphics.failed());
    EXPECT_EQ(hardware->aborted, 1u);
    EXPECT_FALSE(hardware->busy());
    EXPECT_FALSE(hardware->callback.function);
}
TEST_F(GuiGraphics, CancellationWakesBlockedTransferAndDrainsCallback)
{
    auto hardware = std::make_shared<Graphics>();
    hardware->mode = Graphics::Mode::Lost;
    std::array<std::uint16_t, 16> pixels{};
    std::stop_source stop;
    ui::gui::GraphicsDMA graphics(hardware, stop.get_token());
    auto cancellation =
      runtime::thread::create({ .name = "gui.cancel", .priority = 10, .stack_size = 8192 }, [&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{ 5 });
        stop.request_stop();
    });
    const auto start = std::chrono::steady_clock::now();
    graphics.addToQueue(operation(pixels));
    cancellation.join();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds{ 100 });
    EXPECT_FALSE(hardware->busy());
    EXPECT_FALSE(hardware->callback.function);
    EXPECT_EQ(hardware->aborted, 1u);
}

TEST(FrameTimer, CountsMultipleHardwareWraps)
{
    auto hardware = std::make_shared<hal::Timer>(hal::Timer::Configuration{ 1000000, 0, 9999 });
    ASSERT_TRUE(hardware->start());
    ui::gui::util::FrameTimer timer(hardware);
    const auto nativeTime = [] {
        timespec t{};
        EXPECT_EQ(clock_gettime(CLOCK_MONOTONIC, &t), 0);
        return std::chrono::seconds{ t.tv_sec } + std::chrono::nanoseconds{ t.tv_nsec };
    };
    const auto beforeStart = nativeTime();
    timer.start();
    const auto afterStart = nativeTime();
    std::this_thread::sleep_for(std::chrono::milliseconds{ 75 });
    const auto beforeEnd = nativeTime();
    const auto elapsed = timer.elapsed();
    const auto afterEnd = nativeTime();
    // Hosted ThreadX sleep ticks are not a wall-clock reference under load.
    // Compare against the native clock used by the simulated hardware counter.
    EXPECT_GE(elapsed, beforeEnd - afterStart - std::chrono::microseconds{ 1 });
    EXPECT_LE(elapsed, afterEnd - beforeStart + std::chrono::microseconds{ 1 });
    EXPECT_GT(elapsed, std::chrono::milliseconds{ 30 });
    ASSERT_TRUE(hardware->stop());
}

TEST(DirtyRegions, NestedAndAdjacentDamageIsCopiedOnce)
{
    ui::gui::util::DirtyRegions dirty;
    dirty.add({ 10, 20, 100, 40 });
    dirty.add({ 20, 30, 10, 10 });
    dirty.add({ 110, 20, 100, 40 });
    ASSERT_EQ(dirty.regions().size(), 1U);
    EXPECT_EQ(dirty.regions()[0], (touchgfx::Rect{ 10, 20, 200, 40 }));
    dirty.clear();
    EXPECT_TRUE(dirty.regions().empty());
}

TEST(DirtyRegions, OverflowRetainsCoverageWithoutFullScreenFallback)
{
    ui::gui::util::DirtyRegions dirty;
    for (unsigned i = 0; i < 17; ++i)
        dirty.add({ static_cast<int16_t>(i * 30), 10, 10, 10 });
    ASSERT_LE(dirty.regions().size(), 16U);
    unsigned pixels{};
    for (const auto& r : dirty.regions())
        pixels += unsigned(r.width) * r.height;
    EXPECT_LT(pixels, 3000U);
    for (int i = 0; i < 17; ++i) {
        bool covered{};
        for (const auto& r : dirty.regions())
            covered |= r.x <= i * 30 && r.right() >= i * 30 + 10 && r.y <= 10 && r.bottom() >= 20;
        EXPECT_TRUE(covered);
    }
}

TEST(DirtyRegions, RandomizedCoverageClippingAndDisjointCopies)
{
    std::mt19937 random{ 3729 };
    for (unsigned trial = 0; trial < 100; ++trial) {
        ui::gui::util::DirtyRegions dirty;
        std::array<touchgfx::Rect, 50> input{};
        for (auto& r : input) {
            r = { static_cast<int16_t>(int(random() % 1100) - 50),
                  static_cast<int16_t>(int(random() % 650) - 25),
                  static_cast<int16_t>(random() % 60),
                  static_cast<int16_t>(random() % 50) };
            dirty.add(r);
            r.restrictTo(1024, 600);
        }
        EXPECT_LE(dirty.regions().size(), 16U);
        for (const auto& r : input) {
            if (r.isEmpty())
                continue;
            // The algorithm merges whole rectangles, never splits one.
            bool covered{};
            for (const auto& out : dirty.regions())
                covered |=
                  out.x <= r.x && out.y <= r.y && out.right() >= r.right() && out.bottom() >= r.bottom();
            EXPECT_TRUE(covered);
        }
        for (unsigned i = 0; i < dirty.regions().size(); ++i) {
            const auto& a = dirty.regions()[i];
            EXPECT_GE(a.x, 0);
            EXPECT_GE(a.y, 0);
            EXPECT_LE(a.right(), 1024);
            EXPECT_LE(a.bottom(), 600);
            for (unsigned j = i + 1; j < dirty.regions().size(); ++j) {
                auto overlap = a;
                overlap &= dirty.regions()[j];
                EXPECT_TRUE(overlap.isEmpty());
            }
        }
    }
}
