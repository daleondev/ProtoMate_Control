#pragma once
#include <array>
#include <span>
#include <touchgfx/hal/Types.hpp>

namespace ui::gui::util
{
    // Bounded, conservative damage coverage. Copy only the union once where
    // possible; on overflow merge the cheapest pair instead of copying a screen.
    class DirtyRegions
    {
      public:
        void add(touchgfx::Rect region)
        {
            region.restrictTo(1024, 600);
            if (region.isEmpty())
                return;
            for (unsigned i = 0; i < m_count;) {
                auto overlap = region;
                overlap &= m_regions[i];
                const auto combined = unite(region, m_regions[i]);
                if (!overlap.isEmpty() || area(combined) == area(region) + area(m_regions[i])) {
                    region = combined;
                    m_regions[i] = m_regions[--m_count];
                    i = 0;
                }
                else
                    ++i;
            }
            if (m_count == m_regions.size()) {
                // The seventeenth region participates in the cost comparison.
                unsigned a = 0, b = m_count;
                unsigned cost = ~0U;
                for (unsigned i = 0; i < m_count; ++i) {
                    for (unsigned j = i + 1; j <= m_count; ++j) {
                        const auto other = j == m_count ? region : m_regions[j];
                        const auto extra =
                          area(unite(m_regions[i], other)) - area(m_regions[i]) - area(other);
                        if (extra < cost) {
                            cost = extra;
                            a = i;
                            b = j;
                        }
                    }
                }
                if (b == m_count) {
                    region = unite(region, m_regions[a]);
                    m_regions[a] = m_regions[--m_count];
                }
                else {
                    const auto combined = unite(m_regions[a], m_regions[b]);
                    m_regions[b] = m_regions[--m_count];
                    m_regions[a] = m_regions[--m_count];
                    add(combined);
                }
                add(region);
                return;
            }
            m_regions[m_count++] = region;
        }
        std::span<const touchgfx::Rect> regions() const { return { m_regions.data(), m_count }; }
        void clear() { m_count = 0; }

      private:
        static unsigned area(const touchgfx::Rect& r) { return unsigned(r.width) * unsigned(r.height); }
        static touchgfx::Rect unite(touchgfx::Rect a, const touchgfx::Rect& b)
        {
            a.expandToFit(b);
            return a;
        }
        std::array<touchgfx::Rect, 16> m_regions{};
        unsigned m_count{};
    };
}
