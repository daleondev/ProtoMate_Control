# Common is also built directly by hardware examples, without src/CMakeLists.txt.
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wpedantic>")

add_subdirectory(app)
add_subdirectory(icc)
add_subdirectory(motion)
add_subdirectory(util)

add_library(proto_common STATIC)
add_library(proto::common ALIAS proto_common)

target_include_directories(proto_common 
    PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(proto_common 
    PUBLIC 
        project_compiler_settings
        pneumo::coroutines
    PRIVATE
        proto::common::app
        proto::common::icc
        proto::common::motion
        proto::common::util
)

if(TARGET runtime::cxx_abi)
    target_link_libraries(proto_common PUBLIC runtime::cxx_abi)
endif()

