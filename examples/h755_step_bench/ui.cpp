#include "Link.hpp"
#include "hal/hal.hpp"

#include <cstdio>
#include <exception>

int main()
{
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    std::setvbuf(stdin, nullptr, _IONBF, 0);
    try {
        bench::Link link{ bench::Publisher::UI };
        std::atomic_bool busy_load{};
        std::atomic_uint32_t sent{}, echoes{}, errors{};
        auto echo = link.bus.topic<^^bench::Topics::bench_echo>().subscribe([&](const bench::Load& packet) {
            ++echoes;
            if (packet != bench::packet(packet.sequence)) {
                ++errors;
            }
        }, pnm::msg::SubscriptionOptions::latestOnly());
        auto worker = runtime::thread::create_jthread(
          { .name = "bench.load", .priority = 10, .stack_size = 8192 }, [&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                if (busy_load) {
                    link.bus.topic<^^bench::Topics::bench_load>().publish(bench::packet(++sent));
                }
                echo.poll();
                // Planned application state rate, with a 256-byte payload each way.
                std::this_thread::sleep_for(20ms);
            }
        });
        bench::Report result;
        auto reports = link.bus.topic<^^bench::Topics::bench_report>().subscribe(
          [&](const bench::Report& value) { result = value; });
        const auto deadline = Clock::now() + 5s;
        while (!link.connected() && Clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
        }
        if (!link.connected()) {
            hal::panic("STEP bench ICC handshake failed");
        }
        std::uint32_t sequence{};
        std::puts("[step-bench] READY M7 console -> ICC -> M4 STEP; EN_N remains HIGH");
        for (;;) {
            std::puts("[step-bench] 1 tiny, 2 independent, 3 100kHz, 4 update, 5 restart, 6 wrap, 7 "
                      "underrun, 8 ICC load, 9 profile; h help");
            std::fflush(stdout);
            const auto input = std::getchar();
            if (input == EOF) {
                std::clearerr(stdin);
                std::this_thread::sleep_for(10ms);
                continue;
            }
            if (input < '1' || input > '9') {
                continue;
            }
            const auto test = static_cast<std::uint32_t>(input - '0');
            if (!link.connected()) {
                std::puts("[step-bench] FAIL: ICC disconnected; reset both cores");
                continue;
            }
            const auto before = echoes.load();
            const auto prior_errors = errors.load();
            busy_load = test == 8;
            if (busy_load) {
                std::this_thread::sleep_for(100ms);
            }
            std::printf("[step-bench] START case=%lu sequence=%lu\n",
                        static_cast<unsigned long>(test),
                        static_cast<unsigned long>(++sequence));
            std::fflush(stdout);
            link.bus.topic<^^bench::Topics::bench_command>().publish(bench::Command{ sequence, test });
            const auto until = Clock::now() + 8s;
            while (result.sequence != sequence && Clock::now() < until) {
                reports.poll(100ms);
            }
            busy_load = false;
            if (result.sequence != sequence) {
                std::printf("[step-bench] FAIL: missing M4 result; connected=%u sent=%lu echoes=%lu; reset "
                            "both cores\n",
                            unsigned(link.connected()),
                            static_cast<unsigned long>(sent.load()),
                            static_cast<unsigned long>(echoes.load()));
                continue;
            }
            const auto verified = echoes.load() - before;
            const auto errors_seen = errors.load() - prior_errors;
            const bool passed = result.passed && !errors_seen && (test != 8 || verified >= 10);
            std::printf("[step-bench] %s case=%lu check=%lu pulses=%llu,%llu,%llu completions=%lu,%lu,%lu "
                        "progress=%lu changed_at=%llu guard=%lu load=%lu echoes=%lu errors=%lu\n",
                        passed ? "PASS" : "FAIL",
                        static_cast<unsigned long>(test),
                        static_cast<unsigned long>(result.check),
                        static_cast<unsigned long long>(result.pulses[0]),
                        static_cast<unsigned long long>(result.pulses[1]),
                        static_cast<unsigned long long>(result.pulses[2]),
                        static_cast<unsigned long>(result.completions[0]),
                        static_cast<unsigned long>(result.completions[1]),
                        static_cast<unsigned long>(result.completions[2]),
                        static_cast<unsigned long>(result.progress),
                        static_cast<unsigned long long>(result.changed_at),
                        static_cast<unsigned long>(result.guard_stopped),
                        static_cast<unsigned long>(result.load_received),
                        static_cast<unsigned long>(verified),
                        static_cast<unsigned long>(errors_seen + result.load_errors));
            std::fflush(stdout);
        }
    } catch (const std::exception& error) {
        hal::panic("M7 STEP bench", error.what());
    }
}
