# Also applies when the STEP bench image builds icc without the rest of src/.
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wpedantic>")

add_subdirectory(bridge)
add_subdirectory(transport)
add_subdirectory(catalog)

add_library(application_icc STATIC)
add_library(application::icc ALIAS application_icc)
target_include_directories(application_icc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(application_icc
    PUBLIC
        pneumo::coroutines
    PRIVATE
        application::icc::bridge
        application::icc::transport
        application::icc::catalog
)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    # Coroutine and messaging templates otherwise exceed embedded flash in Debug.
    # Keep debug information and useful stepping, as for the control endpoints.
    target_compile_options(application_icc PRIVATE $<$<CONFIG:Debug>:-Og>)
endif()

# The application selects the threading ABI; the transport has no HAL dependency.
if(TARGET runtime::cxx_abi)
    target_link_libraries(application_icc PUBLIC runtime::cxx_abi)
endif()


if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(examples)
    if(BUILD_TESTING)
        add_subdirectory(tests)
    endif()
endif()
