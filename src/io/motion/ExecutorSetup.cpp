#include "Executor.hpp"
#include <stdexcept>
#include <utility>

namespace io::motion
{
    Executor::Executor(std::shared_ptr<hal::IStepGenerator> generator,
                       Prepare prepare,
                       std::pmr::memory_resource* memory)
      : m_generator{ std::move(generator) }
      , m_prepare{ std::move(prepare) }
      , m_memory{ memory }
    {
        if (!m_generator || !m_prepare || !m_memory) {
            throw std::invalid_argument("executor hardware required");
        }
        for (unsigned i{ 0 }; i < 3; ++i) {
            m_outputs[i] = m_generator->output(hal::step::Axis(i));
        }
    }
}
