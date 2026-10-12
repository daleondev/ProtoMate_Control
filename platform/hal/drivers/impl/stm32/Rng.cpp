#include "Rng.hpp"
#include "hal/stm32/HalResult.hpp"
#include "hal/stm32/HardwareSemaphore.hpp"

namespace hal
{
    auto Rng::generate() noexcept -> util::Result<Value>
    {
        const std::scoped_lock local(m_mutex);
        const stm32::HardwareSemaphore shared(4);
        if (!shared) { return make_error_result<Value>(HalError::Timeout); }
        Value value{};
        const auto status = HAL_RNG_GenerateRandomNumber(&m_handle, &value);
        if (status != HAL_OK) { return make_error_result<Value>(static_cast<HalError>(status)); }
        return value;
    }
}
