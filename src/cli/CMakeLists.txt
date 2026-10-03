add_library(application_cli STATIC)
add_library(application::cli ALIAS application_cli)

target_sources(application_cli
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/thread.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Parser.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Parser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/filesystem.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/filesystem.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/utilities.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/utilities.cpp
)

target_include_directories(application_cli
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(application_cli
    PUBLIC
        project_compiler_settings
    PRIVATE
        pneumo::meta
        runtime::runtime
)

target_compile_options(application_cli
    PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CURSES_NEED_WIDE TRUE)
    find_package(Curses REQUIRED)

    target_sources(application_cli
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Console.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Console.cpp
    )

    target_compile_definitions(application_cli
        PRIVATE
            _XOPEN_SOURCE_EXTENDED=1
    )

    target_include_directories(application_cli
        PRIVATE
            ${CURSES_INCLUDE_DIRS}
    )

    target_link_libraries(application_cli
        PRIVATE
            ${CURSES_LIBRARY}
            ${CURSES_EXTRA_LIBRARY}
    )
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
