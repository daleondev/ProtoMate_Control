// Test-only uncalibrated geometry; never linked into normal firmware.
#include "ui/app/Application.hpp"
#include "ui/motion/RobotConfiguration.hpp"
#include "common/icc/transport/InterCoreTransport.hpp"
#include "hal/core/core.hpp"
int main(){
    using namespace pnm::units::literals;
    ui::motion::setSimulationConfigurationProvider([](const auto& h){
        auto c=ui::motion::simulationConfiguration(h);
        c.rotary[0].motor_revolutions_per_axis_revolution=5;
        c.rotary[1].motor_revolutions_per_axis_revolution=5;
        c.rotary[0].axis_reference=70_deg;c.rotary[1].axis_reference=70_deg;
        c.linear.travel_per_output_revolution=1.25_mm;c.linear.axis_reference=20_mm;
        c.linear_dynamics={4_mm_s,30_mm_s2,30_mm_s2,300_mm_s3};
        for(auto& d:c.rotary_dynamics)d={20_deg/1_s,100_deg_s2,100_deg_s2,1000_deg_s3};
        c.geometry.limits=ui::motion::ScaraKinematics::JointLimits{{-100_deg,10_deg,0_mm},{70_deg,70_deg,20_mm}};
        c.workspace_minimum={-600_mm,-600_mm,0_mm};c.workspace_maximum={600_mm,600_mm,20_mm};
        for(auto& home:c.homing){home.seek=5_rpm;home.latch=.5_rpm;home.timeout=10_s;}
        return std::optional{c};
    });
    icc::InterCoreTransport transport{hal::core::create_intercore_channel()};
    auto application=std::make_unique<ui::Application>(transport);application->start();application->join();
}
