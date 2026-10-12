add_library(proto_io_motion INTERFACE)
add_library(proto::io::motion ALIAS proto_io_motion)

target_sources(proto_io_motion
    INTERFACE
        Defaults.hpp
        Limits.hpp
        Controller.hpp
        Controller.cpp
        StepperMotor.hpp
        StepperMotor.cpp 
        Sequence.hpp
        Sequence.cpp 
)

target_link_libraries(proto_io_motion 
    INTERFACE
        platform::hal
)
