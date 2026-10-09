#pragma once

#include <mutex>
#if defined(HAL_PLATFORM_LINUX) && defined(HAL_STEP_THREADX)
#include "hal/linux/ThreadMutex.hpp"
#elif defined(HAL_PLATFORM_LINUX)
#include "hal/linux/Mutex.hpp"
#endif

namespace hal::device::util
{
#if defined(HAL_PLATFORM_LINUX) && defined(HAL_STEP_THREADX)
    using DriverMutex = linux::ThreadMutex;
#elif defined(HAL_PLATFORM_LINUX)
    using DriverMutex = linux::Mutex;
#else
    using DriverMutex = std::mutex;
#endif
}
