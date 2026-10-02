add_library(runtime INTERFACE)
add_library(runtime::runtime ALIAS runtime)

add_subdirectory(libc)
add_subdirectory(libstdcxx)

target_link_libraries(runtime
    INTERFACE
        runtime::libc
        runtime::libstdcxx
        project_compiler_settings
)

function(runtime_attach target_name)
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR "Unknown runtime executable target: ${target_name}")
    endif()

    target_sources(${target_name}
        PRIVATE
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/startup.cpp
    )

    target_link_libraries(${target_name}
        PRIVATE
            runtime::runtime
            platform::hal
    )

    target_link_options(${target_name}
        PRIVATE
            LINKER:--wrap=main
    )
endfunction()

runtime_attach(${PROJECT_NAME})

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(tests)
endif()
