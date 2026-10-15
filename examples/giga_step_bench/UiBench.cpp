#include "Link.hpp"
#include "hal/console.hpp"
#include "hal/hal.hpp"
#include "ui/gui/Controller.hpp"

#include <cstdio>
#include <exception>

int main()
{
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    hal::console::initialize();
    std::setvbuf(stdin, nullptr, _IONBF, 0);
    try {
        bench::Link link{ bench::Publisher::UI };
        ui::gui::Controller gui;
        auto graphics =
          runtime::thread::create_jthread({ .name = "bench.gui", .priority = 8, .stack_size = 32768 },
                                          [&](std::stop_token stop) { static_cast<void>(gui.run(stop)); });
        auto touch =
          runtime::thread::create_jthread({ .name = "bench.touch", .priority = 7, .stack_size = 8192 },
                                          [&](std::stop_token stop) { gui.acquireTouch(stop); });
        volatile std::uint32_t qspi_checksum{};
        std::atomic_bool busy_load{};
        std::atomic_uint32_t sent{}, echoes{}, errors{};
        std::atomic_uint32_t ping_count{}, ping_errors{}, ping_max_us{};
        auto echo = link.bus.topic<^^bench::Topics::bench_echo>().subscribe([&](const bench::Load& packet) {
            ++echoes;
            if (packet != bench::packet(packet.sequence)) {
                ++errors;
            }
        }, pnm::msg::SubscriptionOptions::latestOnly());
        // Publication must not inherit the duration of graphics/QSPI work.
        auto traffic = runtime::thread::create_jthread(
          { .name = "bench.traffic", .priority = 9, .stack_size = 8192 }, [&](std::stop_token stop) {
            auto next = Clock::now();
            while (!stop.stop_requested()) {
                if (busy_load) {
                    link.bus.topic<^^bench::Topics::bench_load>().publish(bench::packet(++sent));
                }
                echo.poll();
                next += 20ms;
                if (next < Clock::now()) {
                    next = Clock::now();
                }
                std::this_thread::sleep_until(next);
            }
        });
        auto control = runtime::thread::create_jthread(
          { .name = "bench.control", .priority = 8, .stack_size = 8192 }, [&](std::stop_token stop) {
            std::uint32_t challenge{};
            while (!stop.stop_requested()) {
                if (busy_load) {
                    const auto started = Clock::now();
                    auto call = link.bus.service<^^bench::Services::bench_ping>().request(++challenge, 500ms);
                    const auto reply = call.get();
                    const auto elapsed = static_cast<std::uint32_t>(
                      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count());
                    if (!reply || *reply != (challenge ^ 0xA55AA55Au)) {
                        ++ping_errors;
                    }
                    else {
                        ++ping_count;
                    }
                    if (elapsed > ping_max_us.load()) {
                        ping_max_us = elapsed;
                    }
                }
                std::this_thread::sleep_for(200ms);
            }
        });
        auto worker = runtime::thread::create_jthread(
          { .name = "bench.load", .priority = 10, .stack_size = 8192 }, [&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                if (busy_load) {
                    auto* flash = reinterpret_cast<const volatile std::uint32_t*>(0x90800000u);
                    std::uint32_t sum{};
                    for (unsigned i = 0; i < 65536; ++i) {
                        sum ^= flash[i];
                    }
                    qspi_checksum = sum;
                    std::puts("[step-bench] USB/ICC/QSPI/DMA2D/LTDC load "
                              "active........................................");
                }
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
        std::puts("[step-bench] 1 tiny, 2 independent, 3 100kHz, 4 update, 5 restart, 6 wrap, 7 "
                  "underrun, 8 combined load, 9 profile; e ESC identification/100000 reads");
        std::fflush(stdout);
        for (;;) {
            const auto character = hal::console::try_read();
            if (character.state != hal::console::InputState::Character) {
                std::this_thread::sleep_for(10ms);
                continue;
            }
            const auto input = character.character;
            if ((input < '1' || input > '9') && input != 'e') {
                continue;
            }
            const auto test = input == 'e' ? 10u : static_cast<std::uint32_t>(input - '0');
            if (!link.connected()) {
                std::puts("[step-bench] FAIL: ICC disconnected; reset both cores");
                continue;
            }
            const auto before = echoes.load();
            const auto sent_before = sent.load();
            const auto load_started = Clock::now();
            const auto prior_errors = errors.load();
            const auto prior_ping_count = ping_count.load();
            const auto prior_ping_errors = ping_errors.load();
            busy_load = test == 8;
            if (busy_load) {
                std::this_thread::sleep_for(100ms);
            }
            std::printf("[step-bench] START case=%lu sequence=%lu\n",
                        static_cast<unsigned long>(test),
                        static_cast<unsigned long>(++sequence));
            std::fflush(stdout);
            link.bus.topic<^^bench::Topics::bench_command>().publish(bench::Command{ sequence, test });
            const auto until = Clock::now() + (test == 10 ? 60s : 8s);
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
            const auto pings = ping_count.load() - prior_ping_count;
            const auto bad_pings = ping_errors.load() - prior_ping_errors;
            const bool passed = result.passed && !errors_seen &&
                                gui.status().state == ui::gui::State::Running && !gui.status().errors &&
                                !gui.status().underruns &&
                                (test != 8 || (verified >= 10 && pings != 0 && bad_pings == 0));
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
            std::printf("[step-bench] service_max=%lu cycles (240 MHz); stop=%lu cycles; LTDC underruns=%lu; "
                        "QSPI=%lu\n",
                        static_cast<unsigned long>(result.service_cycles),
                        static_cast<unsigned long>(result.stop_cycles),
                        static_cast<unsigned long>(gui.status().underruns),
                        static_cast<unsigned long>(qspi_checksum));
            const auto guiStatus = gui.status();
            std::printf(
              "[step-bench] GUI frames=%lu presented=%lu late=%lu max_us=%lu errors=%lu verified=%u\n",
              static_cast<unsigned long>(guiStatus.frames),
              static_cast<unsigned long>(guiStatus.presented),
              static_cast<unsigned long>(guiStatus.missedDeadlines),
              static_cast<unsigned long>(guiStatus.maximumRenderUs),
              static_cast<unsigned long>(guiStatus.errors),
              guiStatus.graphicsVerified);
            if (test == 8) {
                const auto stats = link.bridge.topicStatistics("bench/load");
                std::printf(
                  "[step-bench] ICC published=%lu elapsed_ms=%lld outgoing_dropped=%llu connected=%u "
                  "urgent_replies=%lu urgent_errors=%lu urgent_max_us=%lu\n",
                  static_cast<unsigned long>(sent.load() - sent_before),
                  static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - load_started)
                      .count()),
                  static_cast<unsigned long long>(stats ? stats->dropped : 0),
                  unsigned(link.connected()),
                  static_cast<unsigned long>(pings),
                  static_cast<unsigned long>(bad_pings),
                  static_cast<unsigned long>(ping_max_us.load()));
            }
            std::fflush(stdout);
        }
    } catch (const std::exception& error) {
        hal::panic("M7 STEP bench", error.what());
    }
}
