if(NOT DEFINED HAL_BOARD)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(HAL_BOARD linux)
    else()
        message(FATAL_ERROR "The hardware platform must select its board")
    endif()
endif()
add_subdirectory(${HAL_BOARD})

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
