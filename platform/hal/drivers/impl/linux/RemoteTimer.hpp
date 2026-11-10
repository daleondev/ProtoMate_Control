#pragma once
#include "hal/drivers/itf/ITimer.hpp"
#include "simulation/Client.hpp"
namespace hal
{
    class RemoteTimer final : public ITimer
    {
      public:
        RemoteTimer(const RemoteTimer&) = delete;
        auto operator=(const RemoteTimer&) -> RemoteTimer& = delete;
        RemoteTimer(RemoteTimer&&) = delete;
        auto operator=(RemoteTimer&&) -> RemoteTimer& = delete;

        RemoteTimer(unsigned id, std::uint32_t hz, Tick prescaler, Tick reload)
          : m_id{ id }
        {
            simulation::client::Transaction transaction{};
            simulation::client::command(simulation::Operation::TimerConfigure,
                                        { id, 0, hz, prescaler, reload });
            simulation::client::register_timer(
              id, this, [](void* p) { static_cast<RemoteTimer*>(p)->dispatch(); });
        }
        ~RemoteTimer() override
        {
            simulation::client::Transaction transaction{};
            static_cast<void>(stop());
            simulation::client::register_timer(m_id, nullptr, nullptr);
        }
        util::Result<> start() noexcept override { return run(false); }
        util::Result<> startIt() noexcept override { return run(true); }
        util::Result<> stopIt() noexcept override { return stop(); }
        util::Result<> stop() noexcept override
        {
            simulation::client::Transaction transaction{};
            return result(simulation::client::command(simulation::Operation::TimerStop, { m_id }));
        }
        Tick getCounter() const noexcept override
        {
            simulation::client::Transaction transaction{};
            if (!simulation::client::in_interrupt()) {
                simulation::client::command(simulation::Operation::TimerRead, { m_id });
            }
            return field(0);
        }
        void setCounter(Tick value) noexcept override { write(1, value); }
        Tick getAutoReload() const noexcept override { return field(2); }
        void setAutoReload(Tick value) noexcept override { write(3, value); }
        Tick getPrescaler() const noexcept override { return field(1); }
        void setPrescaler(Tick value) noexcept override { write(2, value); }
        void forceUpdateEvent() noexcept override { setCounter(0); }
        void reset() noexcept override { setCounter(0); }
        std::uint32_t getInputFrequencyHz() const noexcept override { return field(3); }
        std::uint32_t getTickFrequencyHz() const noexcept override
        {
            return static_cast<std::uint32_t>(getInputFrequencyHz() / (std::uint64_t{ getPrescaler() } + 1));
        }
        bool isRunning() const noexcept override { return field(4) != 0; }
        std::uint32_t getStateVersion() const noexcept override { return field(VERSION_FIELD); }
        std::chrono::nanoseconds getElapsedTime() const noexcept override
        {
            const auto hz{ getTickFrequencyHz() };
            return std::chrono::nanoseconds{ hz ? std::uint64_t{ getCounter() } * NS_PER_SECOND / hz : 0 };
        }
        void setPeriodElapsedCallback(PeriodElapsedCallback callback) noexcept override
        {
            simulation::client::Transaction transaction{};
            m_callback = std::move(callback);
            m_observed = field(EVENT_FIELD);
        }

      private:
        static constexpr unsigned VERSION_FIELD{ 5 }, EVENT_FIELD{ 6 };
        static constexpr auto NS_PER_SECOND{ std::chrono::nanoseconds{ std::chrono::seconds{ 1 } }.count() };
        Tick durationToTicksImpl(std::chrono::nanoseconds duration) const noexcept override
        {
            const auto hz{ getTickFrequencyHz() };
            if (duration.count() <= 0 || !hz) {
                return 0;
            }
            const long double ticks{ static_cast<long double>(duration.count()) * hz / NS_PER_SECOND };
            return ticks >= UINT32_MAX ? UINT32_MAX
                                       : static_cast<Tick>(ticks) + (ticks > static_cast<Tick>(ticks));
        }
        void setPeriodImpl(std::chrono::nanoseconds duration) noexcept override
        {
            const auto ticks{ durationToTicksImpl(duration) };
            write(4, ticks ? ticks - 1 : 0);
        }
        void write(unsigned kind, Tick value) noexcept
        {
            simulation::client::Transaction transaction{};
            simulation::client::command(simulation::Operation::TimerConfigure, { m_id, kind, value });
            m_observed = field(EVENT_FIELD);
        }
        util::Result<> run(bool interrupt) noexcept
        {
            simulation::client::Transaction transaction{};
            m_observed = field(EVENT_FIELD);
            return result(
              simulation::client::command(simulation::Operation::TimerStart, { m_id, interrupt }));
        }
        static util::Result<> result(const simulation::Packet& p)
        {
            if (p.status) {
                return std::unexpected(
                  std::error_code{ static_cast<int>(p.status), std::generic_category() });
            }
            return {};
        }
        std::uint32_t field(unsigned i) const noexcept
        {
            return simulation::acquire(simulation::client::memory()->timers[m_id][i]);
        }
        void dispatch() noexcept
        {
            const auto current{ field(EVENT_FIELD) };
            if (current != m_observed) {
                m_observed = current;
                if (m_callback) {
                    m_callback();
                }
            }
        }
        unsigned m_id;
        std::uint32_t m_observed{};
        PeriodElapsedCallback m_callback;
    };
}
