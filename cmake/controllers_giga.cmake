# Reuse the standalone presets and their output directories. Each core keeps
# its own architecture flags, generated headers, libraries and linker layout.
if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|Release)$")
    message(FATAL_ERROR "Use giga-debug or giga-release for the combined build")
endif()
string(TOLOWER "${CMAKE_BUILD_TYPE}" configuration)

foreach(role io ui)
    set(preset giga-${role}-${configuration})
    if(GIGA_M4_RAM_DEBUG AND role STREQUAL "io")
        set(preset giga-io-ram-debug)
    endif()
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

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(io_build giga-io-${configuration})
if(GIGA_M4_RAM_DEBUG)
    set(io_build giga-io-ram-debug)
endif()
add_custom_target(images ALL
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/giga_images.py
        --m4 ${PROJECT_SOURCE_DIR}/build/${io_build}/io-controller.elf
        --m7 ${PROJECT_SOURCE_DIR}/build/giga-ui-${configuration}/ui-controller.elf
        --output ${PROJECT_BINARY_DIR}/images
    DEPENDS controllers VERBATIM)
add_custom_target(flash
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/giga_program.py flash
        --build ${PROJECT_BINARY_DIR}
    DEPENDS images USES_TERMINAL VERBATIM)
