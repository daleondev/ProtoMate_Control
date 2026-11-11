add_library(proto_ui_motion INTERFACE)
add_library(proto::ui::motion ALIAS proto_ui_motion)

target_sources(proto_ui_motion INTERFACE
    Client.hpp
    Client.cpp
    ClientPrograms.cpp
    ClientHistory.cpp
    ClientControls.cpp
        ClientSupervision.cpp
    Axis.hpp
    AxisConversion.hpp
    AxisConversion.cpp
    ScaraKinematics.cpp
    RobotConfiguration.cpp
    Planner.cpp
    PlannerJog.cpp
    Robot.cpp
    RobotExecution.cpp
    RobotPresentation.cpp
)

target_link_libraries(proto_ui_motion INTERFACE runtime::runtime pneumo::coroutines)
