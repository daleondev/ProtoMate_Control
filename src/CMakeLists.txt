add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wpedantic>")

if(PROTOMATE_OPCUA_SAMPLE)
    add_subdirectory(${PROJECT_SOURCE_DIR}/examples/opcua_sample ${PROJECT_BINARY_DIR}/opcua_sample)
    return()
endif()

add_subdirectory(common)

if(GIGA_MOTION_BENCH)
    add_subdirectory(${PROJECT_SOURCE_DIR}/examples/giga_motion_bench ${PROJECT_BINARY_DIR}/motion_bench)
    return()
endif()

if(GIGA_STEP_BENCH)
    add_subdirectory(${PROJECT_SOURCE_DIR}/examples/giga_step_bench ${PROJECT_BINARY_DIR}/step_bench)
    return()
endif()

if(DEFINED STM32_CORE)
    if(STM32_CORE STREQUAL "CM4")
        set(application_role io)
    else()
        set(application_role ui)
    endif()

    add_subdirectory(${application_role})

    if(GIGA_MOTION_API_TEST)
        target_sources(${PROJECT_NAME} PRIVATE
            ${PROJECT_SOURCE_DIR}/examples/giga_motion_api_test/${application_role}.cpp
            ${PROJECT_SOURCE_DIR}/examples/giga_motion_api_test/Trace.cpp)
    else()
        target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/${application_role}/main.cpp)
    endif()

    target_link_libraries(${PROJECT_NAME} 
        PRIVATE 
            proto::${application_role}
    )

else()
    add_subdirectory(io)
    add_subdirectory(ui)

    target_sources(${PROTOMATE_CONTROLLER_TARGET}
        PRIVATE 
            ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    )

    target_link_libraries(${PROTOMATE_CONTROLLER_TARGET}
        PRIVATE 
            proto::io 
            proto::ui
    )
endif()

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(tests)
endif()
