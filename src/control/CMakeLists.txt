add_library(application_control STATIC)
add_library(application::control ALIAS application_control)

target_sources(application_control
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/thread.cpp
)

target_include_directories(application_control
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(application_control
    PUBLIC
        project_compiler_settings
    PRIVATE
        pneumo::pneumo
        runtime::runtime
)

target_compile_options(application_control
    PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
