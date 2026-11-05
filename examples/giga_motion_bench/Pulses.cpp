#include "Link.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include <atomic>
extern "C" volatile std::uint32_t giga_step_irq_cycles_max;

namespace motion_bench
{
    // Baseline backend regression under communication/GUI/periodic CPU load.
    // This is deliberately NOT a coordinated trajectory execution adapter.
    void pulses(const Request& r, Report& report, std::shared_ptr<hal::IInterCoreChannel> channel)
    {
        auto& endpoint = link(channel);
        endpoint.start();
        if constexpr (SIDE == 0) {
            const auto& enable = motorEnable;
            static auto generator = hal::core::create_stepper_generator();
            static auto stimulus = hal::gpio::create_output({ .pin = { hal::gpio::Port::B, 11 } });
            static auto marker = hal::gpio::create_output({ .pin = { hal::gpio::Port::H, 4 } });
            if (!enable || !generator || !stimulus || !marker)
                hal::panic("bench factory unavailable");
            if (enable->read() != hal::gpio::Level::High)
                hal::panic("bench enable active");
            static std::array<std::shared_ptr<hal::IStepOutput>, 3> axes;
            static std::array<std::shared_ptr<hal::IDigitalOutput>, 3> directions;
            runtime::Notification changed;
            auto handler =
              endpoint.bus.service<^^Services::echo>().serve([&](const std::string& value) { return value; });
            auto watch = pnm::msg::watch(handler, [&] { changed.signal(); });
            std::jthread load;
            if (r.fixture)
                load = runtime::thread::create_jthread(
                  { .name = "bench.reserve", .priority = 6, .stack_size = 4096 }, [&](std::stop_token stop) {
                    auto next = std::chrono::steady_clock::now();
                    while (!stop.stop_requested()) {
                        next += 1ms;
                        stimulus->write(hal::gpio::Level::High);
                        const auto start = cycles();
                        while (cycles() - start < r.fixture * 2400)
                            asm volatile("nop");
                        stimulus->write(hal::gpio::Level::Low);
                        std::this_thread::sleep_until(next);
                    }
                });
            for (unsigned i = 0; i < 3; ++i) {
                const auto id = static_cast<hal::core::MotorId>(i);
                if (!axes[i])
                    axes[i] = hal::core::create_stepper_step_output(generator, id);
                if (!directions[i])
                    directions[i] = hal::core::create_stepper_direction_output(id);
                if (!axes[i] || !directions[i])
                    hal::panic("bench axis unavailable");
                directions[i]->write(hal::gpio::Level::Low);
                if (!axes[i]->prepare({ std::chrono::nanoseconds{ 1'000'000'000 / r.rate }, 5us }, r.count))
                    hal::panic("bench prepare");
            }
            if (!generator->setProgressCallback([&](const auto&) noexcept {
                marker->toggle();
                changed.signal();
            }))
                hal::panic("bench callback");
            if (!generator->start())
                hal::panic("bench start");
            release(shared.armed, acquire(shared.sequence));
            const auto start = now();
            if (!generator->startPrepared({ 1ms, 1ms, 1ms }))
                hal::panic("bench arm");
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::microseconds{ std::uint64_t(r.count) * 1000000 / r.rate } + 2s;
            while (std::chrono::steady_clock::now() < deadline) {
                handler.poll();
                const auto status = generator->status();
                if (status.state == hal::step::State::Underrun ||
                    status.state == hal::step::State::DmaError) {
                    ++report.errors;
                    break;
                }
                if (std::ranges::all_of(status.axes,
                                        [](auto state) { return state == hal::step::State::Completed; }))
                    break;
                changed.waitUntil(std::min(deadline, std::chrono::steady_clock::now() + 1ms));
            }
            const auto status = generator->stop();
            static_cast<void>(generator->setProgressCallback({}));
            if (load.joinable()) {
                load.request_stop();
                load.join();
            }
            report.elapsed = now() - start;
            report.sent = r.count;
            report.received = status.pulses[0];
            report.value = status.pulses[1];
            report.checksum = status.pulses[2];
            report.maximum_backlog = giga_step_irq_cycles_max;
            const auto drain = std::chrono::steady_clock::now() + 1s;
            while (acquire(shared.uiDone) != acquire(shared.sequence) &&
                   std::chrono::steady_clock::now() < drain) {
                handler.poll();
                changed.waitUntil(std::chrono::steady_clock::now() + 1ms);
            }
            if (!status.counts_exact ||
                std::ranges::any_of(status.pulses, [&](auto count) { return count != r.count; }) ||
                enable->read() != hal::gpio::Level::High)
                ++report.errors;
        }
        else {
            while (acquire(shared.armed) != acquire(shared.sequence))
                std::this_thread::sleep_for(1ms);
            auto service = endpoint.bus.service<^^Services::echo>();
            const auto start = now();
            const auto duration = std::uint64_t(r.count) * 1000000 / r.rate;
            unsigned sequence{};
            while (now() - start < duration) {
                auto scheduled = start + sequence * 10000;
                if (!reached(scheduled)) {
                    std::this_thread::sleep_for(1ms);
                    continue;
                }
                const auto before = now();
                std::string data(128, 'x');
                auto result = service.request(data, 100ms).get();
                report.latency.add(now() - scheduled);
                report.processing.add(now() - before);
                if (!result || *result != data)
                    ++report.errors;
                ++sequence;
                ++report.sent;
            }
            report.elapsed = now() - start;
        }
    }
}
