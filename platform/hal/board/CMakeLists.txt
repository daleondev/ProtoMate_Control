add_subdirectory(nucleo_h753zi)

add_library(hal_board STATIC)
add_library(hal::board ALIAS hal_board)

target_link_libraries(hal_board 
    PUBLIC 
        hal::devices
    PRIVATE
        hal::board::nucleo_h753zi
)

target_sources(hal_board
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/board.hpp
)
