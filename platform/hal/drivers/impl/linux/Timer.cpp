#include "Timer.hpp"

#include "hal/drivers/common.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <utility>

namespace hal
{
    namespace
    {
        constexpr std::uint64_t NANOSECONDS_PER_SECOND{ 1'000'000'000ULL };

        class PthreadLock
        {
          public:
            explicit PthreadLock(pthread_mutex_t& mutex) noexcept
              : m_mutex{ mutex }
            {
                if (pthread_mutex_lock(&m_mutex) != 0) {
                    std::terminate();
                }
            }

            ~PthreadLock()
            {
                if (m_ownsLock && pthread_mutex_unlock(&m_mutex) != 0) {
                    std::terminate();
                }
            }

            PthreadLock(const PthreadLock&) = delete;
            PthreadLock& operator=(const PthreadLock&) = delete;
            PthreadLock(PthreadLock&&) = delete;
            PthreadLock& operator=(PthreadLock&&) = delete;

            auto unlock() noexcept -> void
            {
                if (pthread_mutex_unlock(&m_mutex) != 0) {
                    std::terminate();
                }
                m_ownsLock = false;
            }

          private:
            pthread_mutex_t& m_mutex;
            bool m_ownsLock{ true };
        };

        [[nodiscard]] auto monotonic_nanoseconds() noexcept -> std::uint64_t
        {
            timespec now{};
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
                std::terminate();
            }
            return (static_cast<std::uint64_t>(now.tv_sec) * NANOSECONDS_PER_SECOND) +
                   static_cast<std::uint64_t>(now.tv_nsec);
        }

        [[nodiscard]] auto monotonic_deadline(std::uint64_t delay_nanoseconds) noexcept -> timespec
        {
            timespec deadline{};
            if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
                std::terminate();
            }

            const std::uint64_t nanoseconds{ static_cast<std::uint64_t>(deadline.tv_nsec) +
                                             delay_nanoseconds };
            deadline.tv_sec += static_cast<time_t>(nanoseconds / NANOSECONDS_PER_SECOND);
            deadline.tv_nsec = static_cast<long>(nanoseconds % NANOSECONDS_PER_SECOND);
            return deadline;
        }
    }

    Timer::Timer(Configuration configuration) noexcept
      : m_timerInputHz{ configuration.input_frequency_hz }
      , m_prescaler{ configuration.prescaler }
      , m_autoReload{ configuration.auto_reload }
      , m_startedAtNanoseconds{ monotonic_nanoseconds() }
    {
        pthread_condattr_t condition_attributes{};
        if (pthread_mutex_init(&m_mutex, nullptr) != 0 || pthread_condattr_init(&condition_attributes) != 0 ||
            pthread_condattr_setclock(&condition_attributes, CLOCK_MONOTONIC) != 0 ||
            pthread_cond_init(&m_condition, &condition_attributes) != 0) {
            std::terminate();
        }
        static_cast<void>(pthread_condattr_destroy(&condition_attributes));

        if (pthread_create(&m_worker, nullptr, workerEntry, this) != 0) {
            std::terminate();
        }
    }

    Timer::~Timer()
    {
        {
            PthreadLock lock{ m_mutex };
            m_shuttingDown = true;
            notifyWorker();
        }
        if (pthread_join(m_worker, nullptr) != 0 || pthread_cond_destroy(&m_condition) != 0 ||
            pthread_mutex_destroy(&m_mutex) != 0) {
            std::terminate();
        }
    }

    auto Timer::start() noexcept -> util::Result<>
    {
        PthreadLock lock{ m_mutex };
        if (getTickFrequencyHzUnlocked() == 0U) {
            return make_error_result(HalError::Error);
        }

        const auto now{ monotonic_nanoseconds() };
        captureCounter(now);
        m_startedAtNanoseconds = now;
        m_running = true;
        m_interruptEnabled = false;
        ++m_stateVersion;
        notifyWorker();
        return {};
    }

    auto Timer::stop() noexcept -> util::Result<>
    {
        PthreadLock lock{ m_mutex };
        captureCounter(monotonic_nanoseconds());
        m_running = false;
        m_interruptEnabled = false;
        ++m_stateVersion;
        notifyWorker();
        return {};
    }

    auto Timer::startIt() noexcept -> util::Result<>
    {
        PthreadLock lock{ m_mutex };
        if (getTickFrequencyHzUnlocked() == 0U) {
            return make_error_result(HalError::Error);
        }

        const auto now{ monotonic_nanoseconds() };
        captureCounter(now);
        m_startedAtNanoseconds = now;
        m_running = true;
        m_interruptEnabled = true;
        ++m_stateVersion;
        notifyWorker();
        return {};
    }

    auto Timer::stopIt() noexcept -> util::Result<> { return stop(); }

    auto Timer::getCounter() const noexcept -> Tick
    {
        PthreadLock lock{ m_mutex };
        return m_running ? counterAt(monotonic_nanoseconds()) : m_counter;
    }

    auto Timer::setCounter(Tick value) noexcept -> void
    {
        PthreadLock lock{ m_mutex };
        m_counter = value;
        m_startedAtNanoseconds = monotonic_nanoseconds();
        ++m_stateVersion;
        notifyWorker();
    }

    auto Timer::getAutoReload() const noexcept -> Tick
    {
        PthreadLock lock{ m_mutex };
        return m_autoReload;
    }

    auto Timer::setAutoReload(Tick value) noexcept -> void
    {
        PthreadLock lock{ m_mutex };
        const auto now{ monotonic_nanoseconds() };
        captureCounter(now);
        m_autoReload = value;
        m_startedAtNanoseconds = now;
        ++m_stateVersion;
        notifyWorker();
    }

    auto Timer::getPrescaler() const noexcept -> Tick
    {
        PthreadLock lock{ m_mutex };
        return m_prescaler;
    }

    auto Timer::setPrescaler(Tick value) noexcept -> void
    {
        PthreadLock lock{ m_mutex };
        const auto now{ monotonic_nanoseconds() };
        captureCounter(now);
        m_prescaler = value;
        m_startedAtNanoseconds = now;
        ++m_stateVersion;
        notifyWorker();
    }

    auto Timer::forceUpdateEvent() noexcept -> void
    {
        PthreadLock lock{ m_mutex };
        m_counter = 0U;
        m_startedAtNanoseconds = monotonic_nanoseconds();
        ++m_stateVersion;
        notifyWorker();
    }

    auto Timer::reset() noexcept -> void { setCounter(0U); }

    auto Timer::getInputFrequencyHz() const noexcept -> std::uint32_t
    {
        PthreadLock lock{ m_mutex };
        return m_timerInputHz;
    }

    auto Timer::getTickFrequencyHz() const noexcept -> std::uint32_t
    {
        PthreadLock lock{ m_mutex };
        return getTickFrequencyHzUnlocked();
    }

    auto Timer::isRunning() const noexcept -> bool
    {
        PthreadLock lock{ m_mutex };
        return m_running;
    }

    auto Timer::getStateVersion() const noexcept -> std::uint32_t
    {
        PthreadLock lock{ m_mutex };
        return m_stateVersion;
    }

    auto Timer::durationToTicksImpl(std::chrono::nanoseconds duration) const noexcept -> Tick
    {
        PthreadLock lock{ m_mutex };
        return durationToTicksUnlocked(duration);
    }

    auto Timer::durationToTicksUnlocked(std::chrono::nanoseconds duration) const noexcept -> Tick
    {
        const auto nanoseconds{ duration.count() };
        const std::uint64_t frequency{ getTickFrequencyHzUnlocked() };
        if (nanoseconds <= 0 || frequency == 0U) {
            return 0U;
        }

        const auto unsigned_nanoseconds{ static_cast<std::uint64_t>(nanoseconds) };
        const std::uint64_t seconds{ unsigned_nanoseconds / NANOSECONDS_PER_SECOND };
        const std::uint64_t remainder{ unsigned_nanoseconds % NANOSECONDS_PER_SECOND };
        constexpr std::uint64_t maximum_tick{ std::numeric_limits<Tick>::max() };
        if (seconds > maximum_tick / frequency) {
            return std::numeric_limits<Tick>::max();
        }

        const std::uint64_t whole_ticks{ seconds * frequency };
        const std::uint64_t fractional_ticks{ (remainder * frequency + NANOSECONDS_PER_SECOND - 1U) /
                                              NANOSECONDS_PER_SECOND };
        if (fractional_ticks > maximum_tick - whole_ticks) {
            return std::numeric_limits<Tick>::max();
        }
        return static_cast<Tick>(whole_ticks + fractional_ticks);
    }

    auto Timer::setPeriodImpl(std::chrono::nanoseconds duration) noexcept -> void
    {
        PthreadLock lock{ m_mutex };
        const Tick ticks{ durationToTicksUnlocked(duration) };
        m_autoReload = ticks == 0U ? 0U : ticks - 1U;
        m_counter = 0U;
        m_startedAtNanoseconds = monotonic_nanoseconds();
        ++m_stateVersion;
        notifyWorker();
    }

    auto Timer::getElapsedTime() const noexcept -> std::chrono::nanoseconds
    {
        PthreadLock lock{ m_mutex };
        const std::uint64_t frequency{ getTickFrequencyHzUnlocked() };
        if (frequency == 0U) {
            return std::chrono::nanoseconds::zero();
        }
        const Tick counter{ m_running ? counterAt(monotonic_nanoseconds()) : m_counter };
        const std::uint64_t nanoseconds{ static_cast<std::uint64_t>(counter) * NANOSECONDS_PER_SECOND /
                                         frequency };
        return std::chrono::nanoseconds{ static_cast<std::int64_t>(nanoseconds) };
    }

    auto Timer::setPeriodElapsedCallback(PeriodElapsedCallback callback) noexcept -> void
    {
        std::shared_ptr<PeriodElapsedCallback> replacement;
        if (callback) {
            replacement = std::make_shared<PeriodElapsedCallback>(std::move(callback));
        }

        PthreadLock lock{ m_mutex };
        m_periodElapsedCallback = std::move(replacement);
    }

    auto Timer::counterAt(std::uint64_t monotonic_nanoseconds) const noexcept -> Tick
    {
        const std::uint64_t period{ static_cast<std::uint64_t>(m_autoReload) + 1U };
        const std::uint64_t frequency{ getTickFrequencyHzUnlocked() };
        if (frequency == 0U) {
            return m_counter;
        }

        const std::uint64_t elapsed_nanoseconds{ monotonic_nanoseconds - m_startedAtNanoseconds };
        const std::uint64_t seconds{ elapsed_nanoseconds / NANOSECONDS_PER_SECOND };
        const std::uint64_t remainder{ elapsed_nanoseconds % NANOSECONDS_PER_SECOND };
        const std::uint64_t whole_ticks{ (seconds % period) * (frequency % period) % period };
        const std::uint64_t fractional_ticks{ remainder * frequency / NANOSECONDS_PER_SECOND % period };
        const std::uint64_t elapsed_ticks{ (whole_ticks + fractional_ticks) % period };
        return static_cast<Tick>((static_cast<std::uint64_t>(m_counter) + elapsed_ticks) % period);
    }

    auto Timer::getTickFrequencyHzUnlocked() const noexcept -> std::uint32_t
    {
        const std::uint64_t divisor{ static_cast<std::uint64_t>(m_prescaler) + 1U };
        return static_cast<std::uint32_t>(m_timerInputHz / divisor);
    }

    auto Timer::nanosecondsUntilOverflowUnlocked(std::uint64_t monotonic_nanoseconds) const noexcept
      -> std::uint64_t
    {
        const std::uint64_t frequency{ getTickFrequencyHzUnlocked() };
        if (frequency == 0U) {
            return NANOSECONDS_PER_SECOND;
        }

        const std::uint64_t period{ static_cast<std::uint64_t>(m_autoReload) + 1U };
        const std::uint64_t counter{ m_running ? counterAt(monotonic_nanoseconds)
                                               : static_cast<std::uint64_t>(m_counter) % period };
        const std::uint64_t ticks_until_overflow{ period - counter };
        return (ticks_until_overflow * NANOSECONDS_PER_SECOND + frequency - 1U) / frequency;
    }

    auto Timer::captureCounter(std::uint64_t monotonic_nanoseconds) noexcept -> void
    {
        if (m_running) {
            m_counter = counterAt(monotonic_nanoseconds);
        }
    }

    auto Timer::notifyWorker() noexcept -> void
    {
        if (pthread_cond_broadcast(&m_condition) != 0) {
            std::terminate();
        }
    }

    auto Timer::workerLoop() noexcept -> void
    {
        if (pthread_mutex_lock(&m_mutex) != 0) {
            std::terminate();
        }

        while (!m_shuttingDown) {
            while (!m_shuttingDown && (!m_running || !m_interruptEnabled)) {
                if (pthread_cond_wait(&m_condition, &m_mutex) != 0) {
                    std::terminate();
                }
            }
            if (m_shuttingDown) {
                break;
            }

            const auto now{ monotonic_nanoseconds() };
            const timespec deadline{ monotonic_deadline(nanosecondsUntilOverflowUnlocked(now)) };
            const int status{ pthread_cond_timedwait(&m_condition, &m_mutex, &deadline) };
            if (status == 0) {
                continue;
            }
            if (status != ETIMEDOUT) {
                std::terminate();
            }

            auto callback{ m_periodElapsedCallback };
            if (pthread_mutex_unlock(&m_mutex) != 0) {
                std::terminate();
            }
            if (callback && *callback) {
                (*callback)();
            }
            if (pthread_mutex_lock(&m_mutex) != 0) {
                std::terminate();
            }
        }

        if (pthread_mutex_unlock(&m_mutex) != 0) {
            std::terminate();
        }
    }

    auto Timer::workerEntry(void* context) noexcept -> void*
    {
        static_cast<Timer*>(context)->workerLoop();
        return nullptr;
    }
}
