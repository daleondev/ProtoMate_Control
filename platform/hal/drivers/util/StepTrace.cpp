#include "StepTrace.hpp"
#if defined(GIGA_MOTION_API_TEST)
#include "StepHardware.hpp"
#include "hal/stm32/hal.hpp"

namespace hal::util::step_trace
{
    // Outside FAST: instrumentation must not displace execution code. Eight
    // records occupy bounded internal data RAM, independent of either heap.
    namespace
    {
        std::array<Record, 8> history{};
        unsigned serial{}, calibration{};
        Record* current{};
    }
    void mark(unsigned phase) noexcept
    {
        if (!current) {
            return;
        }
        current->ticks[phase] = TIM2->CNT;
        current->present |= 1U << phase;
    }
    void calibrate() noexcept
    {
        if (calibration) {
            return;
        }
        Record scratch;
        current = &scratch;
        const auto start{ TIM2->CNT };
        for (unsigned i{ 0 }; i < 32; ++i) {
            mark(Build);
        }
        calibration = step_distance(start, TIM2->CNT);
        current = nullptr;
    }
    std::uint32_t markCost() noexcept { return calibration; }
    void begin(std::uint64_t job, std::uint32_t origin, std::uint32_t boundary, std::uint32_t cut) noexcept
    {
        current = &history[serial++ % history.size()];
        *current = { .job = job, .sequence = serial, .boundary = boundary, .origin = origin, .cut = cut };
        mark(Build);
    }
    void work(unsigned axis, unsigned generated, unsigned copied, unsigned entries) noexcept
    {
        if (!current) {
            return;
        }
        current->generated[axis] = generated;
        current->copied[axis] = copied;
        current->entries[axis] = entries;
    }
    void failure(unsigned site, std::uint32_t lead) noexcept
    {
        if (current) {
            current->site = site;
            current->lead = lead;
        }
    }
    void finish(int error) noexcept
    {
        if (!current) {
            return;
        }
        current->finished = TIM2->CNT;
        current->error = error;
        current->unchanged = !(current->present & (1U << Axis0 | 1U << Axis1 | 1U << Axis2));
        current = nullptr;
    }
    Record last(std::uint64_t job) noexcept
    {
        Record result;
        for (const auto& r : history) {
            if (r.job == job && r.sequence > result.sequence) {
                result = r;
            }
        }
        return result;
    }
    Record worst(std::uint64_t job) noexcept
    {
        Record result;
        for (const auto& r : history) {
            if (r.job == job && r.finished &&
                (!result.finished || step_distance(r.ticks[Build], r.finished) >
                                       step_distance(result.ticks[Build], result.finished))) {
                result = r;
            }
        }
        return result;
    }
}
#endif
