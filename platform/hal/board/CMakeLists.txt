add_library(hal_board STATIC)
add_library(hal::board ALIAS hal_board)
target_link_libraries(hal_board PUBLIC hal::devices)

target_sources(hal_board
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/nucleo_h753zi/board.cpp
)
