add_library(runtime_libc STATIC)
add_library(runtime::libc ALIAS runtime_libc)

target_sources(runtime_libc
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/libc.c
)
if(RUNTIME_FILESYSTEM)
    target_sources(runtime_libc PRIVATE filesystem.cpp storage_media.cpp)
    target_link_libraries(runtime_libc PUBLIC filex levelx)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    target_sources(runtime_libc PRIVATE unavailable.c)
    target_link_options(runtime_libc INTERFACE LINKER:--undefined=_getentropy)
endif()

target_link_libraries(runtime_libc
    PRIVATE
        platform::hal
    PUBLIC
        threadx
        project_compiler_settings
)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    target_sources(runtime_libc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cpu_clock.c)
    target_include_directories(runtime_libc
        BEFORE PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
endif()

target_include_directories(runtime_libc
    PUBLIC
        ${PROJECT_SOURCE_DIR}/platform/runtime
)

target_compile_features(runtime_libc
    PUBLIC
        c_std_11
)

target_compile_definitions(runtime_libc PRIVATE RUNTIME_STORAGE_REQUIRED=$<BOOL:${RUNTIME_STORAGE_REQUIRED}>)
if(RUNTIME_HARDWARE_STORAGE_STARTUP_TEST)
    target_compile_definitions(runtime_libc PUBLIC RUNTIME_HARDWARE_STORAGE_STARTUP_TEST)
endif()

if(RUNTIME_STORAGE_ERASE_FLASH_ON_BOOT)
    target_compile_definitions(runtime_libc PRIVATE RUNTIME_STORAGE_ERASE_FLASH_ON_BOOT)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    # Runtime hook providers and the FileX-backed syscall adapter have no
    # ordinary application references. Keep both archive members in the link.
    target_link_options(runtime_libc
        INTERFACE
            LINKER:--undefined=runtime_libc_initialize
    )
    if(RUNTIME_FILESYSTEM)
        target_link_options(runtime_libc INTERFACE LINKER:--undefined=runtime_filex_initialize)
    endif()
endif()
