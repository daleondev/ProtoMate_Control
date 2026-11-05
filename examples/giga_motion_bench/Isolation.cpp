#include "Bench.hpp"

namespace motion_bench
{
    // Raw queue isolation: fill both expendable lanes before exchanging urgent
    // frames. No bridge or transport may own the channel during this fixture.
    void isolation(const Request& r, Report& report, hal::IInterCoreChannel& channel)
    {
        using Lane = hal::intercore::Lane;
        std::array<std::byte, 32> frame{};
        for (auto lane : { Lane::Normal, Lane::Background }) {
            for (unsigned i = 0; i < 4; ++i) {
                packet(frame, i, now(), now());
                if (!channel.trySend(frame, lane))
                    ++report.errors;
            }
            auto full = channel.trySend(frame, lane);
            if (full || full.error() != hal::IInterCoreChannel::Error::WouldBlock)
                ++report.errors;
            else
                ++report.rejected;
        }
        // Both sides' lower lanes are full before the scheduled urgent stream.
        while (!reached(r.start))
            std::this_thread::sleep_for(1ms);
        auto urgent = r;
        urgent.lane = 1;
        urgent.direction = 2;
        urgent.bytes = 32;
        urgent.rate = 1000;
        raw(urgent, report, channel);
        for (auto lane : { Lane::Normal, Lane::Background }) {
            for (unsigned i = 0; i < 4; ++i) {
                const auto result = channel.tryReceive(frame, lane);
                if (!result || *result != frame.size() || !valid(frame) || read32(frame, 0) != i)
                    ++report.errors;
            }
        }
        report.elapsed = now() - r.start;
    }
}
