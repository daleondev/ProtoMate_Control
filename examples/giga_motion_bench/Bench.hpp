#pragma once
#include "Metrics.hpp"
#include "hal/core/core.hpp"
#include "hal/hal.hpp"
#include "runtime/synchronization/Notification.hpp"
#include "runtime/thread.hpp"
#include <atomic>
#include <memory>

namespace motion_bench
{
    using namespace std::chrono_literals;
#if defined(CORE_CM4)
    inline constexpr unsigned SIDE = 0;
#else
    inline constexpr unsigned SIDE = 1;
#endif
    inline std::uint32_t now() noexcept { return TIM5->CNT; }
    inline std::uint32_t cycles() noexcept { return DWT->CYCCNT; }
    inline bool reached(std::uint32_t time) noexcept { return std::int32_t(now() - time) >= 0; }
    enum class Operation : std::uint32_t
    {
        Clock = 1,
        Kernel,
        Raw,
        Transport,
        Bridge,
        Stop,
        Pulses,
        Topics,
        Actions,
        Flood,
        Isolation,
        Dispatch,
        FastControl,
        Profile,
        Executor
    };
    struct Request
    {
        Operation operation{};
        std::uint32_t fixture{}, bytes{ 32 }, rate{ 1000 }, count{ 1000 }, direction{}, lane{}, start{},
          trace{}, traffic{};
    };
    struct Report
    {
        Histogram latency, processing, scheduling;
        std::uint32_t sent{}, received{}, rejected{}, errors{}, elapsed{}, value{}, clock{},
          maximum_backlog{}, heap_used{}, heap_free{}, stack_free{}, stack_size{}, cpu_ticks{}, wall_us{},
          minimum_stack_permill{}, refill_cycles{}, refill_budget_ticks{}, executor_build_us{}, executor_splice_us{}, executor_error{}, executor_cut{}, executor_reason{}, executor_request{}, icc_state{}, icc_reason{};
        std::uint64_t checksum{};
        std::array<char, 32> minimum_stack_thread{};
    };
    struct Shared
    {
        std::uint32_t sequence{}, done{}, ready{}, boot{}, armed{}, uiDone{}, fastReady{};
        Request request;
        std::array<Report, 2> report;
    };
    static_assert(sizeof(Shared) <= 8192);
    extern Shared shared;
    inline std::shared_ptr<hal::IDigitalOutput> motorEnable;
    // Bench-only dispatch policy, selected before a bridge is constructed.
    inline unsigned dispatchMode{};
    inline bool protectedAdmission{};
    inline std::uint32_t acquire(std::uint32_t& v)
    {
        return std::atomic_ref(v).load(std::memory_order_acquire);
    }
    inline void release(std::uint32_t& v, std::uint32_t n)
    {
        std::atomic_ref(v).store(n, std::memory_order_release);
    }
    void executorBench(const Request&, Report&, std::shared_ptr<hal::IInterCoreChannel>);
    void profileBench(const Request&, Report&, std::shared_ptr<hal::IInterCoreChannel>);
    void fixedKernel(const Request&, Report&);
    void kernel(const Request&, Report&);
    void isolation(const Request&, Report&, hal::IInterCoreChannel&);
    void raw(const Request&, Report&, hal::IInterCoreChannel&);
    void transport(const Request&, Report&, std::shared_ptr<hal::IInterCoreChannel>);
    void bridge(const Request&, Report&, std::shared_ptr<hal::IInterCoreChannel>);
    void topics(const Request&, Report&, std::shared_ptr<hal::IInterCoreChannel>);
    void actions(const Request&, Report&, std::shared_ptr<hal::IInterCoreChannel>);
    void flood(const Request&, Report&, std::shared_ptr<hal::IInterCoreChannel>);
    void pulses(const Request&, Report&, std::shared_ptr<hal::IInterCoreChannel>);
    void fastControl(const Request&, Report&, std::shared_ptr<hal::IInterCoreChannel>);
    void print(const char* name, const Report&, unsigned core);
}
