#include "hal/linux/DisplayHost.hpp"
#include <SDL.h>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <pthread.h>
#include <thread>

namespace
{
    using namespace std::chrono_literals;
    namespace display = hal::linux::display;
    std::atomic_uint64_t simulatedTime{ 1'000'000'000 };
    std::atomic_bool inputSent{};

    std::uint64_t clock() noexcept { return simulatedTime.load(); }

    template<class Predicate>
    bool until(Predicate ready)
    {
        const auto deadline{ std::chrono::steady_clock::now() + 2s };
        while (!ready()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    void observe(void*, const hal::display::State& state, std::uint32_t)
    {
        if (!state.vsyncs || inputSent.exchange(true)) {
            return;
        }
        SDL_Event event{};
        event.type = SDL_MOUSEBUTTONDOWN;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.x = 200;
        event.button.y = 150;
        EXPECT_EQ(SDL_PushEvent(&event), 1);
    }

    void controller()
    {
        EXPECT_TRUE(display::acquire(clock));
        EXPECT_TRUE(until([] { return display::snapshot().vsyncs == 1; }));
        EXPECT_TRUE(until([] { return display::pointer().down; }));
        EXPECT_EQ(display::pointer().time_ns, simulatedTime.load());

        // Wall time and SDL input continue while the simulated peripheral stops.
        // No VSYNC/presentation is allowed until the same peripheral clock advances.
        EXPECT_TRUE(display::submit(1));
        std::this_thread::sleep_for(80ms);
        EXPECT_EQ(display::snapshot().vsyncs, 1);
        EXPECT_EQ(display::snapshot().presentations, 0);
        EXPECT_EQ(display::snapshot().pending, 1);
        simulatedTime.fetch_add(16'000'000);
        std::this_thread::sleep_for(40ms);
        EXPECT_EQ(display::snapshot().vsyncs, 1);
        simulatedTime.fetch_add(666'667);
        EXPECT_TRUE(until([] { return display::snapshot().presentations == 1; }));
        EXPECT_EQ(display::snapshot().active, 1);
        EXPECT_FALSE(display::snapshot().pending);

        // Reacquiring starts a fresh timeline, even if its epoch moves backwards.
        display::release();
        simulatedTime = 0;
        EXPECT_TRUE(display::acquire(clock));
        EXPECT_TRUE(until([] { return display::snapshot().vsyncs == 1; }));
        display::release();

        // Ordinary hosted tests without a native plant retain wall-clock refresh.
        EXPECT_TRUE(display::acquire());
        EXPECT_TRUE(until([] { return display::snapshot().vsyncs >= 2; }));
        display::release();
        display::completed(0);
        pthread_exit(nullptr);
    }
}

TEST(SimulatedDisplay, RefreshAndPointerUsePeripheralClock)
{
    EXPECT_EQ(display::run(controller, observe), 0);
    EXPECT_TRUE(display::cleanup_complete());
}
