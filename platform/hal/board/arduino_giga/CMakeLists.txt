add_library(hal_board_arduino_giga INTERFACE)
add_library(hal::board::arduino_giga ALIAS hal_board_arduino_giga)
target_sources(hal_board_arduino_giga INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/display.cpp ${CMAKE_CURRENT_SOURCE_DIR}/ethercat.cpp ${CMAKE_CURRENT_SOURCE_DIR}/board.cpp ${CMAKE_CURRENT_SOURCE_DIR}/steppers.cpp)
