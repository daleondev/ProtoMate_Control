#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <span>

namespace motion_bench
{
    // Eight bins/octave. Percentiles are conservative bin upper bounds; the
    // exact observed maximum and total are retained independently.
    struct Histogram
    {
        std::array<std::uint32_t, 256> bins{};
        std::uint64_t sum{};
        std::uint32_t count{}, maximum{};
        static constexpr unsigned index(std::uint32_t v) noexcept
        {
            if (v < 8)
                return v;
            const auto shift = std::bit_width(v) - 4U;
            return std::min<std::uint32_t>(255U, 8U + shift * 8U + ((v >> shift) - 8U));
        }
        static constexpr std::uint32_t upper(unsigned bin) noexcept
        {
            if (bin < 8)
                return bin;
            const auto shift = (bin - 8) / 8;
            if (shift >= 29)
                return UINT32_MAX;
            return ((9U + (bin - 8) % 8) << shift) - 1U;
        }
        void add(std::uint32_t us) noexcept
        {
            ++bins[index(us)];
            ++count;
            sum += us;
            maximum = std::max(maximum, us);
        }
        std::uint32_t percentile(unsigned per_mille) const noexcept
        {
            if (!count)
                return 0;
            const auto target = (std::uint64_t(count) * per_mille + 999) / 1000;
            std::uint64_t seen{};
            for (unsigned i{}; i < bins.size(); ++i) {
                seen += bins[i];
                if (seen >= target)
                    return std::min(maximum, upper(i));
            }
            return maximum;
        }
    };
    inline std::uint32_t read32(std::span<const std::byte> data, unsigned offset)
    {
        std::uint32_t value{};
        for (unsigned i{}; i < 4; ++i)
            value |= std::to_integer<std::uint32_t>(data[offset + i]) << (i * 8);
        return value;
    }
    inline void write32(std::span<std::byte> data, unsigned offset, std::uint32_t value)
    {
        for (unsigned i{}; i < 4; ++i)
            data[offset + i] = std::byte(value >> (i * 8));
    }
    inline void packet(std::span<std::byte> data,
                       std::uint32_t sequence,
                       std::uint32_t scheduled,
                       std::uint32_t submitted)
    {
        for (unsigned i{}; i < data.size(); ++i)
            data[i] = std::byte((sequence * 31 + i * 17) & 255);
        write32(data, 0, sequence);
        write32(data, 4, scheduled);
        write32(data, 8, submitted);
        write32(data, 12, sequence ^ std::rotl(scheduled, 7) ^ std::rotl(submitted, 19) ^ 0x93b5d7f1U);
    }
    inline bool valid(std::span<const std::byte> data)
    {
        if (data.size() < 16)
            return false;
        const auto sequence = read32(data, 0);
        if (read32(data, 12) !=
            (sequence ^ std::rotl(read32(data, 4), 7) ^ std::rotl(read32(data, 8), 19) ^ 0x93b5d7f1U))
            return false;
        for (unsigned i = 16; i < data.size(); ++i)
            if (data[i] != std::byte((sequence * 31 + i * 17) & 255))
                return false;
        return true;
    }
}
