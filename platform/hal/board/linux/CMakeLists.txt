add_library(hal_board_linux INTERFACE)
add_library(hal::board::linux ALIAS hal_board_linux)
target_sources(hal_board_linux INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/board.cpp)
