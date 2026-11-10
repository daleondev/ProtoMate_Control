#include "Robot.hpp"
#include "common/icc/catalog/services.hpp"
#include "common/motion/PulseCursor.hpp"
#include <algorithm>
#include <atomic>
#include <new>
#include <stdexcept>

namespace ui::motion
{
    namespace tr = ::motion::trajectory;
    namespace
    {
        constexpr std::uint64_t PREVIEW_PULSE_BUDGET{ 1'000'000 };
        constexpr auto REQUEST_TIMEOUT{ std::chrono::seconds{ 3 } };
#if defined(CORE_CM7)
        constexpr std::size_t ARENA_BYTES{ 248 * 1024 }, ARENA_ALIGNMENT{ 32 };
        // Raw, explicitly initialized POD storage. No startup access to SDRAM,
        // no heap/mutex/runtime objects in the reserved motion region.
        alignas(ARENA_ALIGNMENT)
          // Linker-reserved planning arena; atomic claim guarantees exclusive Robot ownership.
          // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
          __attribute__((section(".motion_sdram"))) std::array<std::byte, ARENA_BYTES> arena_storage{};
        // Linker-reserved planning arena; atomic claim guarantees exclusive Robot ownership.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        std::atomic_bool arena_claimed{};
#endif
        RobotError validate_pulses(const Planner& planner,
                                   const tr::Capabilities& hardware,
                                   const tr::Counts& start,
                                   const std::stop_token& stop)
        {
            tr::PulseCursor cursor{ hardware, start };
            std::uint64_t pulse_budget{ PREVIEW_PULSE_BUDGET };
            for (const auto& segment : planner.segments()) {
                const auto error{ cursor.append(segment) };
                if (error != RobotError::None) {
                    return error;
                }
                for (;;) {
                    if (stop.stop_requested()) {
                        return RobotError::Cancelled;
                    }
                    const auto pulse{ cursor.next() };
                    if (!pulse) {
                        return pulse.error();
                    }
                    if (!*pulse) {
                        break;
                    }
                    if (!pulse_budget--) {
                        return RobotError::Capacity;
                    }
                }
            }
            return RobotError::None;
        }
        bool terminal(Robot::JobState state)
        {
            return state != Robot::JobState::Queued && state != Robot::JobState::Planning;
        }
    }
    std::unique_ptr<Robot::Arena, Robot::ReleaseArena> Robot::reserveArena()
    {
#if defined(CORE_CM7)
        static_assert(sizeof(Arena) <= sizeof(arena_storage));
        if (arena_claimed.exchange(true)) {
            throw std::logic_error("robot planning arena already owned");
        }
        // Placement construction in the linker-reserved, correctly aligned planning arena.
        return std::unique_ptr<Arena, ReleaseArena>{ std::construct_at(
          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
          reinterpret_cast<Arena*>(arena_storage.data())) };
#else
        return std::unique_ptr<Arena, ReleaseArena>{ new Arena{} };
#endif
    }
    void Robot::ReleaseArena::operator()(Arena* arena) const noexcept
    {
#if defined(CORE_CM7)
        std::destroy_at(arena);
        arena_claimed = false;
#else
        std::default_delete<Arena>{}(arena);
#endif
    }
    Robot::Robot(pnm::msg::Bus& bus)
      : m_service{ bus.service<^^icc::Services::io_trajectory>() }
      , m_arena{ reserveArena() }
    {
    }
    Robot::Robot(pnm::msg::Bus& bus, Client& client)
      : Robot{ bus }
    {
        m_client = &client;
    }
    Robot::~Robot() = default; // Application joins the planning worker first.
    void Robot::setConnected(bool connected)
    {
        std::optional<std::stop_source> cancel{};
        {
            std::scoped_lock lock{ m_mutex };
            m_connected = connected;
            if (!connected) {
                cancel.emplace(m_activeStop);
                for (auto& slot : m_arena->slots) {
                    if (!terminal(slot.job.state)) {
                        slot.job.state = JobState::Unknown;
                        slot.job.error = RobotError::Unknown;
                    }
                }
            }
        }
        if (cancel) {
            cancel->request_stop();
        }
        m_changed.signal();
    }
    std::expected<std::uint64_t, RobotError> Robot::preview(PreviewRequest r)
    {
        std::scoped_lock lock{ m_mutex };
        if (!m_connected || m_stopped) {
            return std::unexpected(RobotError::Unknown);
        }
        if (r.model > Model::FixtureArchived) {
            return std::unexpected(RobotError::InvalidInput);
        }
        auto* selected{ m_arena->slots.end() };
        for (auto* it{ m_arena->slots.begin() }; it != m_arena->slots.end(); ++it) {
            if (terminal(it->job.state) &&
                (selected == m_arena->slots.end() || it->job.id < selected->job.id)) {
                selected = it;
            }
        }
        if (selected == m_arena->slots.end()) {
            return std::unexpected(RobotError::Capacity);
        }
        if (!m_next) {
            return std::unexpected(RobotError::Capacity);
        }
        const auto id{ m_next++ };
        r.move.job = id;
        selected->request = r;
        selected->job = { .id = id, .state = JobState::Queued, .model = r.model };
        m_changed.signal();
        return id;
    }
    RobotError Robot::cancel(std::uint64_t id)
    {
        std::unique_lock lock{ m_mutex };
        for (auto& slot : m_arena->slots) {
            if (slot.job.id == id && id) {
                if (terminal(slot.job.state)) {
                    return RobotError::TooLate;
                }
                slot.job.state = JobState::Cancelled;
                slot.job.error = RobotError::Cancelled;
                auto cancel{ id == m_activeJob ? m_activeStop : std::stop_source{ std::nostopstate } };
                lock.unlock(); // Stop callbacks may wake or re-enter consumers.
                cancel.request_stop();
                m_changed.signal();
                return RobotError::None;
            }
        }
        return RobotError::InvalidInput;
    }
    std::optional<Robot::Job> Robot::job(std::uint64_t id) const
    {
        std::scoped_lock lock{ m_mutex };
        for (const auto& slot : m_arena->slots) {
            if (id && slot.job.id == id) {
                return slot.job;
            }
        }
        return {};
    }
    std::array<std::uint64_t, tr::COMMANDS> Robot::jobs() const
    {
        std::scoped_lock lock{ m_mutex };
        std::array<std::uint64_t, tr::COMMANDS> out{};
        for (std::size_t i{ 0 }; i < out.size(); ++i) {
            out[i] = m_arena->slots[i].job.id;
        }
        return out;
    }
    Robot::Job Robot::calculate(PreviewRequest r, std::uint64_t id, const std::stop_token& stop)
    {
        Job result{ .id = id, .state = JobState::Rejected, .model = r.model };
        auto call{ m_service.request(tr::encode(tr::Request{}), REQUEST_TIMEOUT, stop) };
        const auto response{ call.get() };
        if (!response) {
            result.state = JobState::Unknown;
            result.error = RobotError::Unknown;
            return result;
        }
        const auto decoded{ tr::decode_reply(*response) };
        if (!decoded) {
            result.error = decoded.error();
            return result;
        }
        if (decoded->error != RobotError::None) {
            result.error = decoded->error;
            return result;
        }
        const auto configuration{ r.model == Model::Physical
                                    ? board_robot_configuration()
                                    : std::optional{ simulation_configuration(
                                        decoded->capabilities, r.model == Model::FixtureArchived) } };
        if (!configuration) {
            result.error = RobotError::Configuration;
            return result;
        }
        try {
            RobotModel model{ *configuration, decoded->capabilities };
            tr::Counts start{};
            const auto p{ model.toMotors(r.start) };
            for (unsigned i{ 0 }; i < 3; ++i) {
                const auto count{ tr::quantize(p[i]) };
                if (!count) {
                    result.error = count.error();
                    return result;
                }
                start[i] = *count;
            }
            Planner planner{ model, m_arena->workspace, stop };
            result.error = planner.prepare(r.move, start);
            if (result.error != RobotError::None) {
                return result;
            }
            const auto compiled{ planner.compile() };
            if (!compiled) {
                result.error = compiled.error();
                return result;
            }
            // Reject impossible pulse timing rather than stretching the path.
            result.error = validate_pulses(planner, decoded->capabilities, start, stop);
            if (result.error != RobotError::None) {
                if (result.error == RobotError::Cancelled) {
                    result.state = JobState::Cancelled;
                }
                return result;
            }
            tr::Positions endpoint{};
            for (unsigned i{ 0 }; i < 3; ++i) {
                endpoint[i] = static_cast<double>(compiled->endpoint[i]);
            }
            result.endpoint = model.toJoints(endpoint);
            const auto fk{ model.kinematics().forward(result.endpoint) };
            if (!fk) {
                result.error = RobotError::InvalidInput;
                return result;
            }
            result.tcp = fk->position;
            result.plan = *compiled;
            result.state = JobState::Preview;
        } catch (const std::invalid_argument&) {
            result.error = RobotError::Configuration;
        } catch (const std::out_of_range&) {
            result.error = RobotError::PositionLimit;
        }
        return result;
    }
    void Robot::run(const std::stop_token& stop)
    {
        const std::stop_callback cancellation{ stop, [this] { m_changed.signal(); } };
        while (!stop.stop_requested()) {
            refreshPresentationModel(stop);
            if (executePending(stop)) {
                continue;
            }
            PreviewRequest request{};
            std::uint64_t id{ 0 };
            std::stop_source job_stop{ std::nostopstate };
            {
                std::scoped_lock lock{ m_mutex };
                Slot* selected{ nullptr };
                for (auto& slot : m_arena->slots) {
                    if (slot.job.state == JobState::Queued && (!selected || slot.job.id < selected->job.id)) {
                        selected = &slot;
                    }
                }
                if (selected) {
                    selected->job.state = JobState::Planning;
                    request = selected->request;
                    id = selected->job.id;
                    m_activeJob = id;
                    m_activeStop = std::stop_source{};
                    job_stop = m_activeStop;
                }
            }
            if (!id) {
                m_changed.waitUntil(std::chrono::steady_clock::time_point::max());
                continue;
            }
            const std::stop_callback cancel_job{ stop, [&job_stop] { job_stop.request_stop(); } };
            const auto result{ calculate(request, id, job_stop.get_token()) };
            {
                std::scoped_lock lock{ m_mutex };
                m_activeJob = 0;
                m_activeStop = std::stop_source{ std::nostopstate };
                for (auto& slot : m_arena->slots) {
                    if (slot.job.id == id && slot.job.state == JobState::Planning) {
                        slot.job = result;
                        break;
                    }
                }
            }
        }
        std::scoped_lock lock{ m_mutex };
        m_stopped = true;
        for (auto& slot : m_arena->slots) {
            if (!terminal(slot.job.state)) {
                slot.job.state = JobState::Cancelled;
                slot.job.error = RobotError::Cancelled;
            }
        }
    }
}
