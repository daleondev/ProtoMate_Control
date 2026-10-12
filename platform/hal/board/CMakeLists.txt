if(NOT DEFINED HAL_BOARD)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(HAL_BOARD linux)
    else()
        message(FATAL_ERROR "The hardware platform must select its board")
    endif()
endif()
add_subdirectory(${HAL_BOARD})

# Lower-level factories consume a policy contract, not the board factories.
# Select its implementation independently to avoid a driver -> board cycle.
add_library(hal_board_resources STATIC ${HAL_BOARD}/resources.cpp)
add_library(hal::resources ALIAS hal_board_resources)
target_include_directories(hal_board_resources PUBLIC ${PROJECT_SOURCE_DIR}/platform)
target_link_libraries(hal_board_resources PRIVATE platform project_compiler_settings)

add_library(hal_board STATIC)
add_library(hal::board ALIAS hal_board)

target_link_libraries(hal_board 
    PUBLIC 
        hal::devices
    PRIVATE
        hal::board::${HAL_BOARD}
)

target_sources(hal_board
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/board.hpp
)
