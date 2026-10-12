add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wpedantic>")

add_subdirectory(common)
add_subdirectory(io)
add_subdirectory(ui)

target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE application::io application::ui)

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(tests)
endif()
