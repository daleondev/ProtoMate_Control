add_library(hal_board_nucleo_h755ziq INTERFACE)
add_library(hal::board::nucleo_h755ziq ALIAS hal_board_nucleo_h755ziq)
target_sources(hal_board_nucleo_h755ziq INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/board.cpp)
