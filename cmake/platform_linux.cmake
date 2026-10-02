set(THREADX_ARCH linux)
set(THREADX_TOOLCHAIN gnu)
set(THREADX_CUSTOM_PORT ${CMAKE_SOURCE_DIR}/external/threadx_port/linux/gnu)
set(TX_USER_FILE ${PROJECT_SOURCE_DIR}/platform/runtime/tx_user.h)
add_subdirectory(${PROJECT_SOURCE_DIR}/external/threadx)
target_link_libraries(threadx PRIVATE project_compiler_settings)

set(FX_USER_FILE ${PROJECT_SOURCE_DIR}/platform/runtime/fx_user.h)
add_subdirectory(${PROJECT_SOURCE_DIR}/external/filex)
include(${PROJECT_SOURCE_DIR}/cmake/patch_filex.cmake)
runtime_patch_filex()
target_link_libraries(filex PRIVATE project_compiler_settings)

set(LX_USER_FILE ${PROJECT_SOURCE_DIR}/platform/runtime/lx_user.h)
add_subdirectory(${PROJECT_SOURCE_DIR}/external/levelx)
target_link_libraries(levelx PRIVATE project_compiler_settings)
get_target_property(levelx_sources levelx SOURCES)
list(FILTER levelx_sources EXCLUDE REGEX "/(fx_nand_|fx_nor_flash_simulator|lx_nand_|lx_nor_flash_simulator)")
set_property(TARGET levelx PROPERTY SOURCES "${levelx_sources}")

target_compile_definitions(threadx PUBLIC TX_ENABLE_STACK_CHECKING TX_LINUX_MULTI_CORE)

find_package(Threads REQUIRED)

add_library(platform INTERFACE)

target_link_libraries(platform
    INTERFACE
        filex
        levelx
        threadx
        Threads::Threads
        rt
)

target_compile_definitions(platform
    INTERFACE
        $<$<CONFIG:Debug>:DEBUG>
)
