#include "Controller.hpp"
#include <pneumo/logging.hpp>
#include <exception>
namespace io::motion
{
    void Controller::serviceHomeLocked(Group& g)
    {
        namespace tr=::motion::trajectory;
        const auto* owner=m_ownership.find(g.job);
        if(!owner||owner->state==::motion::JobState::Stopping||Ownership::Clock::now()>=owner->deadline||m_faulted.load()){
            if(g.error==tr::Error::None)g.error=m_faulted.load()?tr::Error::Fault:
              owner&&Ownership::Clock::now()>=owner->deadline?tr::Error::LeaseExpired:tr::Error::Cancelled;
            for(unsigned i=0;i<3;++i)if(g.mask&(1U<<i))m_motors[i]->requestStop();
        }
        if(g.home_result.valid()){
            if(!g.home_ready.load())return;
            const auto motor=g.home_order[g.home_stage];
            auto result=StepperMotor::Result::Faulted;
            try{result=g.home_result.get();}catch(...){}
            m_motors[motor]->stopAndWait();
            if(result!=StepperMotor::Result::Completed&&g.error==tr::Error::None)
                g.error=result==StepperMotor::Result::Faulted?tr::Error::Fault:tr::Error::Cancelled;
            ++g.home_stage;
        }
        if(g.error!=tr::Error::None){g.home_finished=true;return;}
        while(g.home_stage<3&&!(g.mask&(1U<<g.home_order[g.home_stage])))++g.home_stage;
        if(g.home_stage==3){g.home_finished=true;return;}
        auto& motor=*m_motors[g.home_order[g.home_stage]];
        const auto& parameters=g.home[g.home_order[g.home_stage]];
        g.home_ready=false;
        try {
            g.home_result=motor.reference(parameters.seek_rpm*1_rpm,parameters.latch_rpm*1_rpm,parameters.timeout*1_s);
            motor.m_lastCompletion.then([this,&g]{g.home_ready=true;m_executionNotification.signal();});
        } catch (const std::exception& error) {
            g.error=tr::Error::Fault;g.home_finished=true;
            try {
                pnm::log::error("Homing job {} could not start motor {}: {}",
                                g.job, unsigned(g.home_order[g.home_stage]) + 1U, error.what());
            } catch (...) {} // A low-memory failure must not depend on logging.
        } catch (...) {
            g.error=tr::Error::Fault;g.home_finished=true;
            try {
                pnm::log::error("Homing job {} could not start motor {}: unknown exception",
                                g.job, unsigned(g.home_order[g.home_stage]) + 1U);
            } catch (...) {}
        }
    }
}
