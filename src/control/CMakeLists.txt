add_library(application_control STATIC)
add_library(application::control ALIAS application_control)

target_sources(application_control
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/thread.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Parser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/filesystem.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/utilities.cpp
)

target_include_directories(application_control
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(application_control
    PUBLIC
        project_compiler_settings
    PRIVATE
        pneumo::meta
        runtime::runtime
)

target_compile_options(application_control
    PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CURSES_NEED_WIDE TRUE)
    find_package(Curses REQUIRED)

    target_sources(application_control
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Console.cpp
    )

    target_compile_definitions(application_control
        PRIVATE
            _XOPEN_SOURCE_EXTENDED=1
    )

    target_include_directories(application_control
        PRIVATE
            ${CURSES_INCLUDE_DIRS}
    )

    target_link_libraries(application_control
        PRIVATE
            ${CURSES_LIBRARY}
            ${CURSES_EXTRA_LIBRARY}
    )
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
