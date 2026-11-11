#include <open62541/types.h>
#include "hal/drivers/factory/rtc.hpp"
#include <chrono>
#include <cerrno>
#include <sys/time.h>

extern "C" UA_DateTime UA_DateTime_nowMonotonic()
{
    const auto elapsed{ std::chrono::steady_clock::now().time_since_epoch() };
    return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 100;
}

extern "C" UA_DateTime UA_DateTime_now()
{
    static const auto rtc{ hal::rtc::create() };
    if (!rtc) return UA_DATETIME_UNIX_EPOCH;
    const auto value{ rtc->getTime() };
    if (!value) return UA_DATETIME_UNIX_EPOCH;
    return UA_DATETIME_UNIX_EPOCH + value->seconds_since_epoch * UA_DATETIME_SEC + value->nanoseconds / 100;
}

extern "C" UA_Int64 UA_DateTime_localTimeUtcOffset() { return 0; }

// The standalone image omits FileX, which currently supplies this Newlib ABI
// entry point together with filesystem calls in the full application.
extern "C" int _gettimeofday(timeval* value, void*)
{
    if (!value) { errno = EFAULT; return -1; }
    const auto time{ UA_DateTime_now() - UA_DATETIME_UNIX_EPOCH };
    value->tv_sec = time / UA_DATETIME_SEC;
    value->tv_usec = (time % UA_DATETIME_SEC) / UA_DATETIME_USEC;
    return 0;
}
