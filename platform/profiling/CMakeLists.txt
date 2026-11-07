add_library(platform_profiling STATIC Recorder.c)
add_library(platform::profiling ALIAS platform_profiling)
target_include_directories(platform_profiling PUBLIC ${PROJECT_SOURCE_DIR}/platform)
target_link_libraries(platform_profiling PRIVATE platform)
target_compile_options(platform_profiling PRIVATE -Wall -Wextra -Wpedantic)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(platform_profiling_unavailable STATIC Unavailable.cpp)
    add_library(platform::profiling_unavailable ALIAS platform_profiling_unavailable)
    target_include_directories(platform_profiling_unavailable PUBLIC ${PROJECT_SOURCE_DIR}/platform)
endif()
if(PROTOMATE_PROFILING)
    target_sources(platform_profiling PRIVATE Native.c)
    target_link_options(platform_profiling INTERFACE LINKER:--undefined=profile_tx_insert)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    target_sources(platform_profiling PRIVATE Allocator.c)
    target_link_options(platform_profiling INTERFACE
        LINKER:--undefined=__wrap__malloc_r LINKER:--undefined=__wrap__calloc_r
        LINKER:--undefined=__wrap__realloc_r LINKER:--undefined=__wrap__memalign_r
        LINKER:--wrap=_malloc_r LINKER:--wrap=_calloc_r
        LINKER:--wrap=_realloc_r LINKER:--wrap=_memalign_r)
endif()
