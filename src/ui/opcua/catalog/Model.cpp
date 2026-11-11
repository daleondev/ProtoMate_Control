#include "Model.hpp"
#include <numbers>
namespace ui::opcua::catalog
{
    namespace
    {
        template<unsigned I> void axis(Axis<I>& out, const services::Axis& in)
        {
            out.commanded = in.commanded; out.feedback = in.feedback; out.velocity = in.velocity;
            out.minimum = in.minimum; out.maximum = in.maximum; out.max_speed = in.max_speed;
            out.referenced = in.referenced; out.feedback_valid = in.valid; out.encoder = in.encoder; out.owner = in.owner;
        }
        template<unsigned I> void motor(Motor<I>& out, const services::Axis& in)
        {
            out.commanded = in.shaft; out.feedback = in.shaft_feedback; out.velocity = in.rpm;
            out.driver = in.driver; out.ready = in.ready; out.fault = in.fault;
            out.fault_code = in.fault_code; out.warning = in.warning; out.reference = in.reference;
            out.valid = in.valid; out.encoder = in.encoder; out.owner = in.owner;
        }
    }
    void Model::update(const services::Snapshot& state)
    {
        system.simulation = state.simulation; system.ui_state = static_cast<app::State>(state.ui_state); system.io_state = static_cast<app::State>(state.io_state);
        system.combined_state = static_cast<app::State>(state.combined_state); system.connected = state.connected;
        system.initialized = state.initialized; system.enabled = state.enabled; system.readiness = state.condition;
        system.boot = state.boot; system.configuration = state.configuration; system.revision = state.sequence; system.age = state.age_ms;
        axis(axes.axis1, state.axes[0]); axis(axes.axis2, state.axes[1]); axis(axes.axis3, state.axes[2]);
        motor(motors.m1, state.motor(0)); motor(motors.m2, state.motor(1)); motor(motors.m3, state.motor(2));
        for (unsigned i{}; i < 3; ++i) {
            io.reference[i] = state.motor(i).reference; io.faults[i] = state.motor(i).fault;
            io.warnings[i] = state.motor(i).warning; io.ready[i] = state.motor(i).ready;
        }
        io.enabled = state.enabled;
        robot.valid = state.feedback_valid; robot.configured = state.robot_configured;
        if (state.feedback_valid) {
            robot.joints = { state.feedback[0] * 180 / std::numbers::pi, state.feedback[1] * 180 / std::numbers::pi, state.feedback[2] * 1000 };
            robot.tcp = state.feedback_tcp; robot.yaw = state.feedback_yaw;
        }
    }
}
