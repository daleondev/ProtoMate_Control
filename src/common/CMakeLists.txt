add_subdirectory(app)
add_subdirectory(icc)

add_library(proto_common STATIC)
add_library(proto::common ALIAS proto_common)

target_include_directories(proto_common 
    PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(proto_common 
    PUBLIC 
        project_compiler_settings
    PRIVATE
        proto::common::app
        proto::common::icc
)

if(TARGET runtime::cxx_abi)
    target_link_libraries(proto_common PUBLIC runtime::cxx_abi)
endif()

