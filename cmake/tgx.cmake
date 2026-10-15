include_guard(GLOBAL)
# Keep the pinned vendor checkout unchanged. Its desktop CMake project changes
# global configuration settings and compiles unused font/2D support.
set(tgx_root "${PROJECT_SOURCE_DIR}/external/tgx")
add_library(proto_tgx STATIC "${tgx_root}/src/Renderer3D.cpp")
add_library(tgx::engine ALIAS proto_tgx)
target_include_directories(proto_tgx SYSTEM PUBLIC "${tgx_root}/src")
set_target_properties(proto_tgx PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_compile_options(proto_tgx PRIVATE -w -O3 -ffunction-sections -fdata-sections)
# Use the same arithmetic paths on the MCU and host. No Arduino runtime,
# texture shader, speculative fast-math assumptions or dynamic mesh caches.
target_compile_definitions(proto_tgx PUBLIC TGX_USE_FAST_INV_TRICK=0
    TGX_USE_FAST_SQRT_TRICK=0 TGX_USE_FAST_INV_SQRT_TRICK=0
    TGX_USE_FMA_MATH=0 TGX_USE_FMA_MATH_MISC=0
    TGX_SHADER_USE_INCREMENTAL_PIXEL_POINTERS=1)
