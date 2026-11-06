add_library(proto_io_motion INTERFACE)
add_library(proto::io::motion ALIAS proto_io_motion)

target_sources(proto_io_motion
    INTERFACE
        Defaults.hpp
        Limits.hpp
        Controller.hpp
        Controller.cpp
    ControllerSetup.cpp
        ControllerAdmission.cpp
        ControllerHoming.cpp
        Ownership.hpp
        Server.hpp
        Server.cpp
        ServerPrograms.cpp
        TrajectorySession.cpp
        Executor.hpp
        Executor.cpp
        ExecutorDiagnostic.cpp
        ScheduleFailure.hpp
        ProfileSchedule.hpp
        ProfileSchedule.cpp
        ProfileCrossing.hpp
        ProfileCurve.hpp
        ProfileMemory.hpp
        StepperMotor.hpp
        StepperMotor.cpp 
        Sequence.hpp
        Sequence.cpp 
)

target_link_libraries(proto_io_motion 
    INTERFACE
        platform::hal
)
