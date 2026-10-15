include(${PROJECT_SOURCE_DIR}/cmake/tgx.cmake)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(asset_dir "${PROJECT_BINARY_DIR}/generated/scene3d")
file(GLOB_RECURSE converter_sources CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/external/tgx/tools/_internal/modules/mesh_pipeline/*.py")
add_custom_command(OUTPUT ${asset_dir}/low.cpp ${asset_dir}/medium.cpp ${asset_dir}/high.cpp
    ${asset_dir}/low.h ${asset_dir}/medium.h ${asset_dir}/high.h
    BYPRODUCTS ${asset_dir}/low.json ${asset_dir}/medium.json ${asset_dir}/high.json
        ${asset_dir}/low.obj ${asset_dir}/medium.obj ${asset_dir}/high.obj
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/scene3d_assets.py
        --tgx ${PROJECT_SOURCE_DIR}/external/tgx --output ${asset_dir} --benchmark
    DEPENDS ${PROJECT_SOURCE_DIR}/scripts/scene3d_assets.py ${converter_sources}
    VERBATIM COMMENT "Converting shaded 3D benchmark meshes (TGX, no textures)")
add_library(proto_scene3d_renderer STATIC Scene.hpp Scene.cpp Renderer.hpp Renderer.cpp Benchmark.cpp
    ${asset_dir}/low.cpp ${asset_dir}/medium.cpp ${asset_dir}/high.cpp)
add_library(proto::ui::gui::scene3d ALIAS proto_scene3d_renderer)
set_target_properties(proto_scene3d_renderer PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_compile_options(proto_scene3d_renderer PRIVATE -O3 -ffunction-sections -fdata-sections)
target_link_libraries(proto_scene3d_renderer PRIVATE tgx::engine)
target_include_directories(proto_scene3d_renderer PRIVATE ${asset_dir})
target_include_directories(proto_scene3d_renderer PUBLIC ${PROJECT_SOURCE_DIR}/src)
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(scene3d_tests tests/renderer_test.cpp)
    target_link_libraries(scene3d_tests PRIVATE proto::ui::gui::scene3d GTest::gtest_main)
    runtime_attach(scene3d_tests)
    add_test(NAME gui.scene3d.renderer COMMAND scene3d_tests)
    set_tests_properties(gui.scene3d.renderer PROPERTIES TIMEOUT 20)
endif()
