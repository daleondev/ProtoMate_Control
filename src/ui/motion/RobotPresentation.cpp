#include "Robot.hpp"
namespace ui::motion
{
    void Robot::requestPresentationModel(std::uint64_t boot, std::uint64_t configuration)
    {
        std::scoped_lock lock{ m_mutex };
        if (m_modelBoot == boot && m_modelConfiguration == configuration)
            return;
        m_modelBoot = boot;
        m_modelConfiguration = configuration;
        m_presentationModel.reset();
        m_modelRequested = boot != 0;
        m_changed.signal();
    }
    std::optional<RobotModel> Robot::presentationModel() const
    {
        std::scoped_lock lock{ m_mutex };
        return m_presentationModel;
    }
    void Robot::refreshPresentationModel(std::stop_token stop)
    {
        std::uint64_t boot{}, configuration{};
        {
            std::scoped_lock lock{ m_mutex };
            if (!m_modelRequested || !m_connected)
                return;
            m_modelRequested = false;
            boot = m_modelBoot;
            configuration = m_modelConfiguration;
        }
        try {
            auto result = [&] {
                try {
                    return model(stop);
                } catch (const std::invalid_argument&) {
                    return model(stop, RobotModel::Scope::Axes);
                }
            }();
            std::scoped_lock lock{ m_mutex };
            if (m_connected && boot == m_modelBoot && configuration == m_modelConfiguration)
                m_presentationModel = std::move(result);
        } catch (const std::exception&) {
            // Missing physical calibration is an ordinary presentation state.
            // Never substitute a preview fixture, and never retry on every redraw.
        }
    }
}
