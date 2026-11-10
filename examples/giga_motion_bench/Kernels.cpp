#include "Bench.hpp"
#include "BrakeCrossing.hpp"
#include "CompactRuns.hpp"
#include "Crossing.hpp"
#include "FastCrossing.hpp"
#include "common/motion/Profile.hpp"
#include "common/motion/PulseCursor.hpp"
#include "ui/motion/Planner.hpp"
#include <numbers>

namespace motion_bench
{
    namespace tr = motion::trajectory;
    namespace
    {
        // POD only; initialized explicitly, never part of bootstrap copy/zero tables.
        __attribute__((section(".motion_bench_workspace"),
                       aligned(32))) ui::motion::Planner::Workspace workspace;
        tr::Capabilities capabilities()
        {
            tr::Capabilities c;
            c.step_radians.fill(std::numbers::pi / 1600);
            c.reference_steps.fill(1200);
            c.limits.fill({ -1e8, 1e8, 100001, 1e9, 1e9, 1e12 });
            c.coordinated_execution = true;
            c.hardware_fingerprint = tr::fingerprint(c);
            return c;
        }
    }
    static void executeKernel(const Request& request, Report& report)
    {
        using namespace pnm::units::literals;
        if (request.fixture == 21 || request.fixture == 22 || request.fixture == 24) {
            fixedKernel(request, report);
            return;
        }
        if (request.fixture >= 5 && request.fixture <= 7) {
            workspace = {};
        }
        const auto cap = capabilities();
        tr::Segment segment{ .ticks = 10'000'000 };
        for (auto& p : segment.motors) {
            p.coefficients = { 0, request.fixture == 9 ? 100000. : 50000. };
        }
        if (request.fixture == 8) {
            segment.ticks = 100'000'000;
            for (auto& p : segment.motors) {
                p.coefficients = { 0, 2000, -2000 };
            }
        }
        tr::PulseCursor cursor{ cap, { 0, 0, 0 } };
        if (cursor.append(segment) != tr::Error::None) {
            ++report.errors;
            return;
        }
        Crossing solver{ { 0, 50000, 10000, 5000, 2000, 1000 }, 10000000 };
        FastCrossing fast{ solver.c, solver.ticks };
        double guess{};
        // Batch fixtures avoid charging a timestamp and histogram update per pulse.
        // They include writing all three axes' timestamps into caller-owned storage.
        __attribute__((section(".motion_bench_buffers"), aligned(32))) static std::array<std::uint32_t, 6000>
          prepared;
        if (request.fixture == 19 || request.fixture == 20) {
            __attribute__((section(".motion_bench_runs"), aligned(32))) static std::array<compact::Program, 3>
              programs;
            const auto prepare = [&] {
                report.value = report.received = report.rejected = 0;
                for (unsigned axis = 0; axis < 3; ++axis) {
                    const auto speed = 100000. - axis * 5000.;
                    const auto profile =
                      motion::Profile::brake(speed, 0, { speed, speed * 10, speed * 10, speed * 100 });
                    if (!profile || !compact::compile(compact::Curve(*profile), programs[axis])) {
                        ++report.errors;
                        return false;
                    }
                    report.value += programs[axis].size * 64;
                    report.received += programs[axis].pulses;
                    report.rejected += programs[axis].failed_fits;
                }
                return true;
            };
            if (request.fixture == 20 && !prepare()) {
                return;
            }
            const auto start = now();
            for (unsigned i = 0; i < request.count; ++i) {
                const auto before = now();
                if (request.fixture == 19) {
                    if (!prepare()) {
                        report.processing.add(now() - before);
                        return;
                    }
                }
                else {
                    for (unsigned axis = 0; axis < 3; ++axis) {
                        const auto first = (i * 251) % (programs[axis].pulses - 256);
                        if (!compact::expand(
                              programs[axis], first, std::span(prepared).subspan(axis * 256, 256))) {
                            ++report.errors;
                        }
                    }
                }
                report.processing.add(now() - before);
                ++report.sent;
                if (request.fixture == 20) {
                    report.checksum += prepared[(i * 173) % 768];
                }
            }
            report.elapsed = now() - start;
            return;
        }
        if (request.fixture == 17 || request.fixture == 18 || request.fixture == 23) {
            const auto start = now();
            for (unsigned repeat = 0; repeat < request.count; ++repeat) {
                const auto before = now();
                for (unsigned axis = 0; axis < 3; ++axis) {
                    const auto maximum = request.fixture == 23 ? double(request.rate) : 100000.;
                    const auto profile =
                      motion::Profile::brake(maximum * (1 - axis * .05),
                                             1234 * maximum / 100000.,
                                             { maximum, maximum * 10, maximum * 10, maximum * 100 });
                    if (!profile) {
                        ++report.errors;
                        return;
                    }
                    double hint{};
                    BrakeCrossing crossing{ *profile };
                    const auto steps = static_cast<unsigned>(std::floor(profile->distance()));
                    for (unsigned i = 1; i <= steps; ++i) {
                        const auto seconds = request.fixture == 18 ? crossing.next(i) * 1e-7
                                                                   : profile->timeAt(i, { .seconds = hint });
                        if (!(seconds > hint)) {
                            ++report.errors;
                            return;
                        }
                        hint = seconds;
                        report.checksum += static_cast<std::uint32_t>(std::ceil(seconds * 10000000));
                    }
                    report.received += steps;
                }
                ++report.sent;
                report.processing.add(now() - before);
            }
            report.value = report.received / request.count * 4;
            report.elapsed = now() - start;
            return;
        }
        if (request.fixture >= 12) {
            constexpr std::array<unsigned, 5> milliseconds{ 1, 2, 5, 10, 20 };
            const auto batch = milliseconds[request.fixture - 12];
            const unsigned pulses = batch * 100;
            std::array<FastCrossing, 3> axes{ { { { { 0, 100000, 10000, 5000, 2000, 1000 } }, 10000000 },
                                                { { { 0, 90000, 20000, 6000, 1000, 500 } }, 10000000 },
                                                { { { 0, 80000, 30000, 7000, 2000, 2000 } }, 10000000 } } };
            const auto started = now();
            for (unsigned repeat = 0; repeat < request.count; ++repeat) {
                const auto before = now();
                for (unsigned axis = 0; axis < 3; ++axis) {
                    double hint{};
                    for (unsigned i = 0; i < pulses; ++i) {
                        auto root = axes[axis].solve(i + .5, 0, 1, hint);
                        prepared[axis * pulses + i] = root.tick;
                        hint = double(root.tick) / axes[axis].ticks +
                               1. / axes[axis].derivative(double(root.tick) / axes[axis].ticks);
                        report.value += root.fallback;
                    }
                }
                report.processing.add(now() - before);
                for (unsigned i = 0; i < 3 * pulses; ++i) {
                    report.checksum += prepared[i];
                }
                report.sent += 3 * pulses;
            }
            report.elapsed = now() - started;
            return;
        }
        const auto started = now();
        for (std::uint32_t i = 0; i < request.count; ++i) {
            const auto before = now();
            bool ok = true;
            switch (request.fixture) {
                case 0: {
                    auto p = motion::Profile::create(10000, { 100000, 1e6, 1e6, 1e7 });
                    ok = p.has_value();
                    if (p) {
                        report.value = static_cast<std::uint32_t>(p->duration() * 1e6);
                    }
                    break;
                }
                case 1: {
                    auto p = motion::Profile::brake(90000, 1234, { 100000, 1e6, 1e6, 1e7 });
                    ok = p.has_value();
                    if (p) {
                        report.value = static_cast<std::uint32_t>(p->duration() * 1e6);
                    }
                    break;
                }
                case 2:
                    ok = tr::validate(segment, cap.limits) == tr::Error::None;
                    break;
                case 3:
                case 8:
                case 9: {
                    auto pulse = cursor.next();
                    if (!pulse) {
                        ++report.errors;
                        report.value = std::to_underlying(pulse.error());
                        report.elapsed = now() - started;
                        return;
                    }
                    if (!*pulse) {
                        report.value = 1;
                        report.elapsed = now() - started;
                        return;
                    }
                    report.checksum ^= (**pulse).tick + std::uint64_t((**pulse).axis) * 0x9E3779B9U;
                    ++report.received;
                    break;
                }
                case 11: {
                    const auto root = fast.solve((i % 60000) + .5, 0, 1, guess);
                    guess =
                      double(root.tick) / fast.ticks + 1. / fast.derivative(double(root.tick) / fast.ticks);
                    report.checksum += root.tick;
                    report.value += root.fallback;
                    report.received += root.evaluations;
                    break;
                }
                case 10: {
                    const auto root = solver.solve((i % 60000) + .5, 0, 1, guess);
                    guess = double(root.tick) / solver.ticks +
                            1. / solver.derivative(double(root.tick) / solver.ticks);
                    report.checksum += root.tick;
                    report.value += root.fallback;
                    report.received += root.evaluations;
                    break;
                }
                case 4: {
                    tr::Request r{ .operation = tr::Operation::Upload, .count = 4 };
                    r.segments.fill(segment);
                    auto packet = tr::encode(r);
                    auto decoded = tr::decodeRequest(packet);
                    ok = decoded && decoded->segments[3].ticks == segment.ticks;
                    report.value = packet.size;
                    break;
                }
                case 5:
                case 6:
                case 7: {
                    ui::motion::RobotModel model{ ui::motion::simulation_configuration(cap), cap };
                    ui::motion::Planner planner{ model, workspace };
                    ui::motion::Move move{ .joints = { 10_deg, 80_deg, 40_mm } };
                    if (request.fixture >= 6) {
                        move.kind = ui::motion::MoveKind::Linear;
                        move.target = { request.fixture == 7 ? 1000_mm : 235_mm, 178_mm, 35_mm };
                    }
                    const auto status = planner.prepare(move, { 1200, 1200, 1200 });
                    if (request.fixture == 7) {
                        ok = status != tr::Error::None;
                        break;
                    }
                    if (status != tr::Error::None) {
                        report.value = std::to_underlying(status);
                        ok = false;
                        break;
                    }
                    const auto plan = planner.compile();
                    ok = plan.has_value();
                    report.value =
                      plan ? static_cast<std::uint32_t>(plan->segments) : std::to_underlying(plan.error());
                    break;
                }
                default:
                    ok = false;
            }
            report.processing.add(now() - before);
            if (!ok) {
                ++report.errors;
            }
            ++report.sent;
        }
        report.elapsed = now() - started;
    }
    void kernel(const Request& request, Report& report)
    {
        // The measured fixed-kernel peak is <14 KiB; 20 KiB leaves >30%
        // headroom on M4. Its optional kernel fixture uses AXI heap storage;
        // production executor profiles own the limited local D2 SRAM instead.
        // M7 retains its reserved stack, without taking space from the UI heap.
        constexpr std::size_t stackSize = (SIDE == 0 ? 20 : 24) * 1024;
        alignas(32) __attribute__((
          section(".bss.motion_bench_stack"))) static std::byte uiStack[SIDE == 0 ? 1 : stackSize];
        std::unique_ptr<std::byte[]> ioStack;
        if constexpr (SIDE == 0) {
            ioStack = std::make_unique<std::byte[]>(stackSize);
        }
        auto* stack = SIDE == 0 ? ioStack.get() : uiStack;
        auto worker =
          runtime::thread::create({ .name = "bench.kernel",
                                    .priority = static_cast<std::int32_t>(request.lane ? request.lane : 15),
                                    .stack_size = stackSize,
                                    .stack_storage = stack },
                                  [&] { executeKernel(request, report); });
        worker.join();
        report.stack_size = stackSize;
        auto words = std::span(reinterpret_cast<const std::uint32_t*>(stack), stackSize / 4);
        for (auto word : words) {
            if (word != 0xefefefefU) {
                break;
            }
            report.stack_free += 4;
        }
    }

}
