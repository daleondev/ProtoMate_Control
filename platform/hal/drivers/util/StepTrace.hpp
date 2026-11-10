#pragma once
#include <array>
#include <cstdint>

// Bounded, test-image-only timing trace. No allocation or formatting in the
// scheduling path; timestamps use the STEP timer's own 100 ns timebase.
#if defined(GIGA_MOTION_API_TEST)
namespace hal::util::step_trace
{
    enum Phase : unsigned
    {
        Build,
        Built,
        Enter,
        RefilledBefore,
        Snapshot,
        Certificate,
        Axis0,
        Generated0,
        Staged0,
        Axis1,
        Generated1,
        Staged1,
        Axis2,
        Generated2,
        Staged2,
        RefilledAfter,
        FinalSample,
        Published,
        PhaseCount
    };
    enum Site : unsigned
    {
        None,
        LeadTime,
        Arm
    };
    struct Record
    {
        std::uint64_t job{};
        std::uint32_t sequence{}, boundary{}, origin{}, cut{};
        std::array<std::uint32_t, PhaseCount> ticks{};
        std::array<std::uint32_t, 3> generated{}, copied{}, entries{};
        std::uint32_t present{}, lead{}, site{}, unchanged{}, finished{};
        int error{};
    };
    void begin(std::uint64_t job, std::uint32_t origin, std::uint32_t boundary, std::uint32_t cut) noexcept;
    void mark(unsigned phase) noexcept;
    void work(unsigned axis, unsigned generated, unsigned copied, unsigned entries) noexcept;
    void failure(unsigned site, std::uint32_t lead) noexcept;
    void finish(int error) noexcept;
    void calibrate() noexcept;
    std::uint32_t markCost() noexcept;        // ticks for 32 mark calls, including their loop.
    Record worst(std::uint64_t job) noexcept; // Slowest in the last eight records, not a lifetime maximum.
    Record last(std::uint64_t job) noexcept;
}
// Must erase the argument tokens when bench-only trace declarations are absent.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define STEP_TRACE(...) ::hal::util::step_trace::__VA_ARGS__
#else
// Must erase the argument tokens when bench-only trace declarations are absent.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define STEP_TRACE(...) ((void)0)
#endif
