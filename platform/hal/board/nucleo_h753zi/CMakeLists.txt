add_library(hal_board_nucleo_h753zi INTERFACE)
add_library(hal::board::nucleo_h753zi ALIAS hal_board_nucleo_h753zi)

target_sources(hal_board_nucleo_h753zi
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/board.cpp
)
