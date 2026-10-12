# Reuse the standalone presets and their output directories. Each core keeps
# its own architecture flags, generated headers, libraries and linker layout.
if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|Release)$")
    message(FATAL_ERROR "Use h755-debug or h755-release for the combined build")
endif()
string(TOLOWER "${CMAKE_BUILD_TYPE}" configuration)

foreach(role io ui)
    set(preset h755-${role}-${configuration})
    add_custom_target(${role}-controller
        COMMAND ${CMAKE_COMMAND} --preset ${preset}
        COMMAND ${CMAKE_COMMAND} --build --preset ${preset}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Building ${role} controller (${CMAKE_BUILD_TYPE})"
        USES_TERMINAL VERBATIM)
endforeach()

# Always invoke the inner builds so source changes and their memory reports
# are handled even when the orchestration project itself has not changed.
add_custom_target(controllers ALL)
add_dependencies(controllers io-controller ui-controller)
