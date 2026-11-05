#include "CompactRuns.hpp"

namespace motion_bench::compact
{
    bool expand(const Program& program, unsigned first, std::span<std::uint32_t> out) noexcept
    {
        if (program.size > CAPACITY || first > program.pulses || out.size() > program.pulses - first)
            return false;
        // Binary search keeps arbitrary DMA-half requests bounded even near
        // the end of a long stop. Only validated immutable programs enter here.
        unsigned lo{}, hi = program.size;
        while (lo < hi) {
            const auto mid = lo + (hi - lo) / 2;
            if (program.runs[mid].first + program.runs[mid].count <= first)
                lo = mid + 1;
            else
                hi = mid;
        }
        for (unsigned r = lo; r < program.size && !out.empty(); ++r) {
            const auto run = program.runs[r];
            if (first < run.first || first >= run.first + run.count)
                return false;
            const unsigned from = first - run.first;
            const unsigned count = std::min<unsigned>(run.count - from, out.size());
            auto time = run.start + from * run.interval +
                        (std::int64_t(from) * (from - (from != 0)) / 2) * run.add;
            auto interval = run.interval + from * run.add;
            for (unsigned n = 0; n < count; ++n) {
                out[n] = ceiling(time) + correction(run, from + n);
                time += interval;
                interval += run.add;
            }
            first += count;
            out = out.subspan(count);
        }
        return out.empty();
    }
}
