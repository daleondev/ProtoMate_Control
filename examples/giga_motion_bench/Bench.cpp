#include "Bench.hpp"
#include "Trace.hpp"
#include "hal/console.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/util/InterCoreMemory.hpp"
#include "hal/util/InterCoreTrace.hpp"
#if defined(CORE_CM7)
#include "ui/gui/Controller.hpp"
#endif
#include "common/diagnostics/Memory.hpp"
#include "common/motion/OperatingEnvelope.hpp"
#include <cstdio>
#include <ctime>
#include <exception>

extern "C" {
std::uint32_t giga_motion_bench_buffer_edges = 512;
}

namespace motion_bench
{
    __attribute__((section(".motion_bench_shared"), aligned(32))) Shared shared;
    void printHistogram(const char* name, const Histogram& h)
    {
        std::printf("\"%s\":{\"count\":%lu,\"mean_us\":%.3f,\"p50_us_upper\":%lu,\"p95_us_upper\":%lu,\"p99_"
                    "us_upper\":%lu,\"p999_us_upper\":%lu,\"max_us\":%lu}",
                    name,
                    (unsigned long)h.count,
                    h.count ? double(h.sum) / h.count : 0.,
                    (unsigned long)h.percentile(500),
                    (unsigned long)h.percentile(950),
                    (unsigned long)h.percentile(990),
                    (unsigned long)h.percentile(999),
                    (unsigned long)h.maximum);
    }
    void print(const char* name, const Report& r, unsigned core)
    {
        std::printf("{\"test\":\"%s\",\"core\":%u,\"sent\":%lu,\"received\":%lu,\"backpressure\":%lu,"
                    "\"errors\":%lu,\"elapsed_us\":%lu,\"value\":%lu,\"checksum\":%llu,",
                    name,
                    core,
                    (unsigned long)r.sent,
                    (unsigned long)r.received,
                    (unsigned long)r.rejected,
                    (unsigned long)r.errors,
                    (unsigned long)r.elapsed,
                    (unsigned long)r.value,
                    (unsigned long long)r.checksum);
        std::printf(
          "\"heap_used\":%lu,\"heap_free\":%lu,\"stack_free\":%lu,\"stack_size\":%lu,\"cpu_10ms_ticks\":%lu,",
          (unsigned long)r.heap_used,
          (unsigned long)r.heap_free,
          (unsigned long)r.stack_free,
          (unsigned long)r.stack_size,
          (unsigned long)r.cpu_ticks);
        std::printf("\"wall_us\":%lu,\"minimum_stack_permill\":%lu,",
                    (unsigned long)r.wall_us,
                    (unsigned long)r.minimum_stack_permill);
        std::printf("\"maximum_backlog\":%lu,", (unsigned long)r.maximum_backlog);
        std::printf("\"auxiliary\":%lu,", (unsigned long)r.clock);
        std::printf("\"refill_cycles\":%lu,\"refill_budget_ticks\":%lu,",
                    (unsigned long)r.refill_cycles,
                    (unsigned long)r.refill_budget_ticks);
        std::printf("\"minimum_stack_thread\":\"%s\",", r.minimum_stack_thread.data());
        std::printf("\"executor_build_us\":%lu,\"executor_splice_us\":%lu,\"executor_error\":%lu,\"executor_"
                    "cut\":%lu,\"executor_reason\":%lu,\"executor_request\":%lu,",
                    (unsigned long)r.executor_build_us,
                    (unsigned long)r.executor_splice_us,
                    (unsigned long)r.executor_error,
                    (unsigned long)r.executor_cut,
                    (unsigned long)r.executor_reason,
                    (unsigned long)r.executor_request);
        std::printf(
          "\"icc_state\":%lu,\"icc_reason\":%lu,", (unsigned long)r.icc_state, (unsigned long)r.icc_reason);
        std::printf("\"startup\":[");
        for (unsigned axis = 0; axis < 3; ++axis) {
            std::printf("%s[", axis ? "," : "");
            for (unsigned n = 0; n < 8; ++n) {
                std::printf("%s%lu", n ? "," : "", (unsigned long)r.startup[axis][n]);
            }
            std::printf("]");
        }
        std::printf("],");
        printHistogram("latency", r.latency);
        std::printf(",");
        printHistogram("processing", r.processing);
        std::printf(",");
        printHistogram("scheduling", r.scheduling);
        std::puts("}");
    }
    void run(const Request& r, Report& report, std::shared_ptr<hal::IInterCoreChannel> channel)
    {
        const auto wholeWindow = now();
        const auto cpu = std::clock();
        beginTrace(r.trace != 0);
        switch (r.operation) {
            case Operation::Dispatch: {
                dispatchMode = r.fixture;
                UINT previous{};
                const UINT priority = dispatchMode ? (SIDE == 0 ? 1U : 2U) : 15U;
                tx_thread_priority_change(tx_thread_identify(), priority, &previous);
                report.value = priority;
                break;
            }
            case Operation::Clock: {
                report.clock = now();
                const auto c = cycles(), t = now();
                std::this_thread::sleep_for(100ms);
                report.elapsed = now() - t;
                report.value = cycles() - c;
                for (unsigned i = 0; i < 1000; ++i) {
                    auto t0 = now();
                    auto t1 = now();
                    report.processing.add(t1 - t0);
                    auto t2 = now();
                    hal::util::intercore_trace(hal::util::InterCoreStage::Handler);
                    report.latency.add(now() - t2);
                }
                break;
            }
            case Operation::Kernel:
                kernel(r, report);
                break;
            case Operation::Isolation:
                isolation(r, report, *channel);
                break;
            case Operation::Raw:
                raw(r, report, *channel);
                break;
            case Operation::Transport:
                transport(r, report, channel);
                break;
            case Operation::Bridge:
                bridge(r, report, channel);
                break;
            case Operation::Pulses:
                pulses(r, report, channel);
                break;
            case Operation::Topics:
                topics(r, report, channel);
                break;
            case Operation::Actions:
                actions(r, report, channel);
                break;
            case Operation::Flood:
                flood(r, report, channel);
                break;
            case Operation::Executor:
                executorBench(r, report, channel);
                break;
            case Operation::Profile:
                profileBench(r, report, channel);
                break;
            case Operation::FastControl:
                fastControl(r, report, channel);
                break;
            default:
                ++report.errors;
        }
        endTrace();
        report.cpu_ticks = std::clock() - cpu;
        report.wall_us = now() - wholeWindow;
        auto heap = diagnostics::collectMemory(diagnostics::MemoryQuery::Heap).heap;
        report.heap_used = heap.used;
        report.heap_free = heap.available;
        auto stacks = diagnostics::collectMemory(diagnostics::MemoryQuery::Stack).stack;
        report.minimum_stack_permill = 1000;
        for (unsigned i = 0; i < stacks.count; ++i) {
            const auto& thread = stacks.threads[i];
            if (thread.stack_size) {
                const auto free = (thread.stack_size - thread.peak_stack_usage) * 1000 / thread.stack_size;
                if (free < report.minimum_stack_permill) {
                    report.minimum_stack_permill = free;
                    report.minimum_stack_thread = thread.name;
                }
            }
        }
    }
}

int main()
{
    using namespace motion_bench;
    try {
#if defined(CORE_CM7)
        shared = {};
        shared.boot = HAL_GetTick() ^ DWT->CYCCNT;
#endif
        auto channel = hal::core::create_intercore_channel();
        auto marker =
          hal::gpio::create_output({ .pin = SIDE == 0 ? hal::gpio::Pin{ hal::gpio::Port::I, 9 }
                                                      : hal::gpio::Pin{ hal::gpio::Port::G, 14 } });
#if defined(CORE_CM4)
        motorEnable = hal::core::create_steppers_enable_output();
        const auto& enable = motorEnable;
        if (!enable || enable->read() != hal::gpio::Level::High) {
            hal::panic("Motion bench motor enable active");
        }
        auto& memory = *reinterpret_cast<hal::util::InterCoreMemory*>(0x38000000U);
        while (acquire(memory.state[1]) != 1) {
            std::this_thread::sleep_for(1ms);
        }
        release(shared.ready, 1);
        std::uint32_t previous{};
        for (;;) {
            const auto next = acquire(shared.sequence);
            if (next == previous) {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            previous = next;
            marker->write(hal::gpio::Level::High);
            const auto request = shared.request;
            auto& report = shared.report[0];
            report = {};
            run(request, report, channel);
            marker->write(hal::gpio::Level::Low);
            if (enable->read() != hal::gpio::Level::High) {
                hal::panic("Motion bench enabled motor");
            }
            release(shared.done, next);
        }
#else
        hal::console::initialize();
        std::setvbuf(stdin, nullptr, _IONBF, 0);
        ui::gui::Controller gui;
        auto graphics =
          runtime::thread::create_jthread({ .name = "bench.gui", .priority = 8, .stack_size = 16U * 1024 },
                                          [&](std::stop_token s) { gui.run(s); });
        auto touch =
          runtime::thread::create_jthread({ .name = "bench.touch", .priority = 7, .stack_size = 8192 },
                                          [&](std::stop_token s) { gui.acquireTouch(s); });
        auto render = runtime::thread::create_jthread(
          { .name = "bench.render", .priority = 16, .stack_size = 16U * 1024 },
          [&](std::stop_token s) { gui.render3d(s); });
        while (!acquire(shared.ready)) {
            std::this_thread::sleep_for(1ms);
        }
        std::printf("[motion-bench] READY boot=%lu TIM5_PSC=%lu ARR=%lu; enable inactive\n",
                    (unsigned long)shared.boot,
                    (unsigned long)TIM5->PSC,
                    (unsigned long)TIM5->ARR);
        std::array<char, 128> line{};
        unsigned length{};
        std::uint32_t sequence{};
        unsigned phase{};
        bool trace{};
        for (;;) {
            auto input = hal::console::try_read();
            if (input.state != hal::console::InputState::Character) {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            if (input.character != '\n' && input.character != '\r') {
                if (length + 1 < line.size()) {
                    line[length++] = input.character;
                }
                continue;
            }
            if (!length) {
                continue;
            }
            line[length] = 0;
            length = 0;
            unsigned priority{}, fixture{}, count{ 1000 }, bytes{ 32 }, rate{ 1000 }, direction{}, lane{};
            Request r;
            const char* name = "clock";
            if (std::string_view(line.data()) == "trace on" || std::string_view(line.data()) == "trace off") {
                trace = line[7] == 'n';
                std::puts("[motion-bench] DONE");
                continue;
            }
            if (std::sscanf(line.data(), "priority %u", &priority) == 1) {
                if (priority < 5 || priority > 20) {
                    std::puts("[motion-bench] invalid priority");
                    continue;
                }
                UINT previous{};
                tx_thread_priority_change(tx_thread_identify(), priority, &previous);
                std::printf(
                  "{\"test\":\"priority\",\"previous\":%u,\"current\":%u}\n", unsigned(previous), priority);
                std::puts("[motion-bench] DONE");
                continue;
            }
            if (std::string_view(line.data()) == "identity") {
                const volatile auto* pair = reinterpret_cast<const volatile unsigned char*>(0x90e00010);
                std::printf("{\"test\":\"identity\",\"boot\":%lu,\"pair_id\":\"", (unsigned long)shared.boot);
                for (unsigned i = 0; i < 32; ++i) {
                    std::printf("%02x", unsigned(pair[i]));
                }
                std::puts("\"}");
                std::puts("[motion-bench] DONE");
                continue;
            }
            unsigned profileShape{}, profileHalf{ 64 }, traffic{};
            if (std::sscanf(line.data(), "startup %u %u %u", &bytes, &direction, &count) == 3) {
                if (!bytes || bytes > 7 || direction > 1 || !count || count > 20) {
                    std::puts("[motion-bench] invalid startup");
                    continue;
                }
                r.operation = Operation::Executor;
                r.startup_mask = bytes;
                r.immutable = direction;
                r.count = count;
                r.rate = motion::operating::STEP_RATE;
                r.lane = 1;
                r.fixture = 0;
                phase = 2;
                name = "executor";
                gui.bars();
                std::this_thread::sleep_for(100ms);
            }
            else if (std::string_view(line.data()) == "gui") {
                gui.showcase();
                std::puts("[motion-bench] DONE");
                continue;
            }
            else if (std::sscanf(line.data(),
                                 "executor %u %u %u %u %u %u",
                                 &count,
                                 &bytes,
                                 &direction,
                                 &fixture,
                                 &profileShape,
                                 &traffic) >= 4) {
                if (!count || count > 1000 || bytes > 500 || direction > 3 || profileShape > 2 ||
                    traffic > 1 || (fixture != 0 && fixture != 20)) {
                    std::puts("[motion-bench] invalid executor");
                    continue;
                }
                r.operation = Operation::Executor;
                r.count = count;
                r.bytes = bytes;
                r.direction = direction;
                r.fixture = fixture;
                r.rate = motion::operating::STEP_RATE;
                r.lane = profileShape;
                r.traffic = traffic;
                phase = 2;
                name = "executor";
            }
            else if (std::sscanf(line.data(),
                                 "profile %u %u %u %u %u",
                                 &rate,
                                 &count,
                                 &fixture,
                                 &profileShape,
                                 &profileHalf) >= 3) {
                if (rate < 1000 || rate > 25000 || !count || count > 1000 || profileShape > 1 ||
                    (profileHalf != 64 && profileHalf != 128 && profileHalf != 256) ||
                    (fixture != 0 && fixture != 20 && fixture != 40)) {
                    std::puts("[motion-bench] invalid profile");
                    continue;
                }
                r.operation = Operation::Profile;
                r.rate = rate;
                r.count = count;
                r.fixture = fixture;
                r.direction = profileShape;
                r.bytes = profileHalf;
                phase = 2;
                name = "profile";
            }
            else if (std::sscanf(line.data(), "stopping %u %u %u", &rate, &count, &fixture) == 3) {
                if (rate < 1000 || rate > 100000 || !count || count > 1000 || fixture > 1) {
                    std::puts("[motion-bench] invalid stopping");
                    continue;
                }
                r.operation = Operation::Kernel;
                r.rate = rate;
                r.count = count;
                r.fixture = 23 + fixture;
                r.lane = 6;
                name = "stopping";
            }
            else if (std::sscanf(line.data(), "fast %u %u %u", &rate, &count, &fixture) == 3) {
                if (!rate || rate > 10000 || !count || count > 1000000 || fixture > 2) {
                    std::puts("[motion-bench] invalid fast");
                    continue;
                }
                r.operation = Operation::FastControl;
                r.rate = rate;
                r.count = count;
                r.fixture = fixture;
                phase = 2;
                name = "fast";
            }
            else if (std::sscanf(line.data(), "dispatch %u", &fixture) == 1) {
                if (phase || fixture > 2) {
                    std::puts("[motion-bench] invalid dispatch; reset first");
                    continue;
                }
                r.operation = Operation::Dispatch;
                r.fixture = fixture;
                name = "dispatch";
            }
            else if (std::sscanf(line.data(), "isolation %u", &count) == 1) {
                if (phase || !count || count > 1000000) {
                    std::puts("[motion-bench] invalid isolation; reset first");
                    continue;
                }
                r.operation = Operation::Isolation;
                r.count = count;
                name = "isolation";
            }
            else if (std::sscanf(line.data(), "flood %u", &count) == 1) {
                if (!count || count > 1000) {
                    std::puts("[motion-bench] invalid flood");
                    continue;
                }
                r.operation = Operation::Flood;
                r.count = count;
                phase = 2;
                name = "flood";
            }
            else if (std::sscanf(line.data(), "topics %u %u %u", &bytes, &rate, &count) == 3) {
                if (bytes < 16 || bytes > 2000 || rate < 100 || rate > 10000 || !count || count > 10000) {
                    std::puts("[motion-bench] invalid topics");
                    continue;
                }
                r.operation = Operation::Topics;
                r.bytes = bytes;
                r.rate = rate;
                r.count = count;
                phase = 2;
                name = "topics";
            }
            else if (std::sscanf(line.data(), "actions %u %u", &count, &fixture) == 2) {
                if (!count || count > 1000 || fixture > 1) {
                    std::puts("[motion-bench] invalid actions");
                    continue;
                }
                r.operation = Operation::Actions;
                r.count = count;
                r.fixture = fixture;
                phase = 2;
                name = "actions";
            }
            else if (std::sscanf(line.data(), "pulses %u %u %u", &rate, &count, &fixture) == 3) {
                if (rate < 100 || rate > 100000 || !count || count > 60000000 ||
                    std::uint64_t(count) * 1000000 / rate > 1200000000 ||
                    (fixture != 0 && fixture != 10 && fixture != 20 && fixture != 40)) {
                    std::puts("[motion-bench] invalid pulses");
                    continue;
                }
                r.operation = Operation::Pulses;
                r.rate = rate;
                r.count = count;
                r.fixture = fixture;
                phase = 2;
                name = "pulses";
            }
            else if (std::sscanf(line.data(), "kernel %u %u %u", &fixture, &count, &lane) >= 2) {
                r.operation = Operation::Kernel;
                r.fixture = fixture;
                r.count = count;
                r.lane = lane;
                name = "kernel";
            }
            else if (std::sscanf(
                       line.data(), "raw %u %u %u %u %u", &bytes, &rate, &count, &direction, &lane) == 5 ||
                     std::sscanf(
                       line.data(), "transport %u %u %u %u %u", &bytes, &rate, &count, &direction, &lane) ==
                       5 ||
                     std::sscanf(
                       line.data(), "bridge %u %u %u %u %u", &bytes, &rate, &count, &direction, &lane) == 5) {
                r.operation = Operation::Raw;
                r.bytes = bytes;
                r.rate = rate;
                r.count = count;
                r.direction = direction;
                r.lane = lane;
                name = "raw";
                unsigned requested{};
                if (line[0] == 't') {
                    r.operation = Operation::Transport;
                    name = "transport";
                    requested = 1;
                }
                if (line[0] == 'b') {
                    r.operation = Operation::Bridge;
                    name = "bridge";
                    requested = 2;
                }
                if (requested < phase || (requested == 2 && (bytes > 2000 || lane > 1 || direction != 1))) {
                    std::puts("[motion-bench] invalid phase or bridge arguments; reset to return to raw");
                    continue;
                }

                if (bytes < 16 || bytes > 2048 || rate > 100000 || count == 0 || count > 1000000 ||
                    (rate && std::uint64_t(count) * 1000000 / rate > 1200000000) || direction > 2 ||
                    lane > 2 || (lane == 2 && bytes > 512)) {
                    std::puts("[motion-bench] invalid");
                    continue;
                }
                phase = requested;
            }
            else if (std::string_view(line.data()) == "clock") {
                r.operation = Operation::Clock;
            }
            else if (std::string_view(line.data()) == "reset") {
                NVIC_SystemReset();
            }
            else if (std::string_view(line.data()) == "status") {
                auto s = gui.status();
                std::printf("{\"test\":\"status\",\"frames\":%lu,\"underruns\":%lu,\"errors\":%lu}\n",
                            (unsigned long)s.frames,
                            (unsigned long)s.underruns,
                            (unsigned long)s.errors);
                continue;
            }
            else {
                std::puts("[motion-bench] clock | kernel fixture count | raw bytes rate count direction lane "
                          "| status | reset");
                continue;
            }
            if (!r.count ||
                (r.operation == Operation::Kernel &&
                 (r.fixture > 24 || r.count > 1000000 || (r.lane != 0 && (r.lane < 5 || r.lane > 20))))) {
                std::puts("[motion-bench] invalid kernel");
                continue;
            }
            r.trace = trace;
            r.start = now() + 20000;
            shared.request = r;
            shared.report[1] = {};
            marker->write(hal::gpio::Level::High);
            release(shared.sequence, ++sequence);
            run(r, shared.report[1], channel);
            release(shared.uiDone, sequence);
            const auto end =
              std::chrono::steady_clock::now() +
              std::chrono::microseconds{ r.rate ? std::uint64_t(r.count) * 1000000 / r.rate : 0 } + 60s;
            while (acquire(shared.done) != sequence && std::chrono::steady_clock::now() < end) {
                std::this_thread::sleep_for(1ms);
            }
            if (acquire(shared.done) != sequence) {
                hal::panic("Motion benchmark peer timeout");
            }
            marker->write(hal::gpio::Level::Low);
            std::printf("{\"test\":\"case\",\"boot\":%lu,\"sequence\":%lu,\"fixture\":%u,\"bytes\":%u,"
                        "\"rate\":%u,\"count\":%u,\"direction\":%u,\"lane\":%u}\n",
                        (unsigned long)shared.boot,
                        (unsigned long)sequence,
                        unsigned(r.fixture),
                        unsigned(r.bytes),
                        unsigned(r.rate),
                        unsigned(r.count),
                        unsigned(r.direction),
                        unsigned(r.lane));
            print(name, shared.report[0], 4);
            print(name, shared.report[1], 7);
            if (trace) {
                printTrace(0);
                printTrace(1);
            }
            std::puts("[motion-bench] DONE");
        }
#endif
    } catch (const std::exception& e) {
        hal::panic("motion bench", e.what());
    }
}
