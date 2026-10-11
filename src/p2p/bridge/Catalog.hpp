#pragma once

#include <meta>
#include <vector>

namespace p2p::util
{
    // Match annotation types independently of top-level const qualification.
    template<typename T>
    consteval auto annotations_of_type(std::meta::info member)
    {
        std::vector<std::meta::info> matches;
        for (const auto annotation : std::meta::annotations_of(member)) {
            if (std::meta::is_same_type(std::meta::remove_const(std::meta::type_of(annotation)),
                                       std::meta::remove_const(^^T)))
                matches.push_back(annotation);
        }
        return matches;
    }
}
