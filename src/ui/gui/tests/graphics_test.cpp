#include "hal/drivers/impl/linux/Timer.hpp"
#include "runtime/thread.hpp"
#include "ui/gui/integration/GraphicsDMA.hpp"
#include "ui/gui/integration/OS.hpp"
#include "ui/gui/util/FrameTimer.hpp"
#include <array>
#include <gtest/gtest.h>
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
    timer.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{ 75 });
    const auto elapsed = timer.elapsed();
    EXPECT_GE(elapsed, std::chrono::milliseconds{ 74 });
    EXPECT_LT(elapsed, std::chrono::milliseconds{ 100 });
    ASSERT_TRUE(hardware->stop());
}
