add_subdirectory(bridge)
add_subdirectory(transport)
add_subdirectory(catalog)

add_library(proto_common_icc INTERFACE)
add_library(proto::common::icc ALIAS proto_common_icc)

target_link_libraries(proto_common_icc
    INTERFACE
        pneumo::coroutines
        proto::common::icc::bridge
        proto::common::icc::transport
        proto::common::icc::catalog
)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    # Coroutine and messaging templates otherwise exceed embedded flash in Debug.
    # Motor service/action routes need size optimization in each 1 MiB bank.
    # Retain Debug symbols; instruction stepping may follow optimized code.
    target_compile_options(proto_common_icc INTERFACE $<$<CONFIG:Debug>:-Os>)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(examples)
    if(BUILD_TESTING)
        add_subdirectory(tests)
    endif()
endif()
