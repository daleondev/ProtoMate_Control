#include "Rtc.hpp"

#include "hal/drivers/common.hpp"

#include <ctime>
#include <utility>

namespace hal
{
    auto Rtc::getTime() const noexcept -> util::Result<Timestamp>
    {
        timespec now{};
        if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_nsec < 0 ||
            std::cmp_greater_equal(now.tv_nsec, NANOSECONDS_PER_SECOND)) {
            return make_error_result<Timestamp>(HalError::Error);
        }

        return Timestamp{ .seconds_since_epoch = static_cast<std::int64_t>(now.tv_sec),
                          .nanoseconds = static_cast<std::uint32_t>(now.tv_nsec) };
    }
}
