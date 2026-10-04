#include "StepHardware.hpp"
#include <thread>

namespace hal::detail
{
    struct LinuxStepHardware::Service
    {
        explicit Service(StepGenerator& generator)
          : worker{ [&generator](std::stop_token stop) {
              while (!stop.stop_requested()) {
                  generator.service();
                  std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
              }
          } }
        {
        }
        std::jthread worker;
    };
    LinuxStepHardware::LinuxStepHardware() { trace_edges = false; }
    LinuxStepHardware::~LinuxStepHardware() { stopService(); }
    void LinuxStepHardware::beginService(StepGenerator& generator)
    {
        m_service = std::make_unique<Service>(generator);
    }
    void LinuxStepHardware::stopService() noexcept { m_service.reset(); }
}
