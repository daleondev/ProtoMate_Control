add_subdirectory(p2p)
add_subdirectory(control)
add_subdirectory(cli)

target_sources(${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/system_threads.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/system_threads.cpp
)

target_include_directories(${PROJECT_NAME} 
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(${PROJECT_NAME} 
    PRIVATE 
        pneumo::units
        application::control
        application::cli
)

# Keep portable bridge/coroutine composition small enough for H753 flash in Debug.
if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/main.cpp TARGET_DIRECTORY ${PROJECT_NAME} PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-Og>")
endif()
