#include "Bench.hpp"
#include "FixedCrossing.hpp"

namespace motion_bench
{
    void fixedKernel(const Request& r, Report& report)
    {
        std::array<motion::Profile, 3> profiles;
        for (unsigned axis = 0; axis < 3; ++axis) {
            const auto maximum = r.fixture == 24 ? double(r.rate) : 100000.;
            auto p = motion::Profile::brake(maximum * (1 - axis * .05), 1234 * maximum / 100000.,
                                           {maximum, maximum * 10, maximum * 10, maximum * 100});
            if (!p) { ++report.errors; return; }
            profiles[axis] = *p;
        }
        const auto setup = now();
        const std::array<FixedCrossing, 3> initial{FixedCrossing(profiles[0]),FixedCrossing(profiles[1]),FixedCrossing(profiles[2])};
        report.scheduling.add(now() - setup);
        __attribute__((section(".motion_bench_buffers"), aligned(32))) static std::array<std::uint32_t, 768> out;
        const auto begin = now();
        for (unsigned iteration = 0; iteration < r.count; ++iteration) {
            auto cursors = initial;
            const auto start = now();
            for (unsigned axis = 0; axis < 3; ++axis) {
                const auto count = r.fixture == 22 ? 256U : unsigned(std::floor(profiles[axis].distance()));
                auto& cursor = cursors[axis];
                if (!cursor.valid()) { ++report.errors; return; }
                std::uint32_t previous{};
                for (unsigned i = 1; i <= count; ++i) {
                    const auto tick = cursor.next(i);
                    if (tick <= previous) ++report.errors;
                    previous = tick;
                    if (r.fixture == 22) out[axis * 256 + i - 1] = tick;
                    else report.checksum += tick;
                }
                report.value += cursor.fallbacks;
                report.received += cursor.evaluations;
                report.sent += count;
            }
            report.processing.add(now() - start);
            if (r.fixture == 22) report.checksum += out[iteration % 768];
        }
        report.elapsed = now() - begin;
    }
}
