add_library(hal_core STATIC)
add_library(hal::core ALIAS hal_core)

target_sources(hal_core PRIVATE core.hpp core.cpp Catalog.hpp)
target_link_libraries(hal_core PUBLIC hal::board)
