include(${PROJECT_SOURCE_DIR}/cmake/tgx.cmake)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(asset_dir "${PROJECT_BINARY_DIR}/generated/scene3d")
file(GLOB_RECURSE converter_sources CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/external/tgx/tools/_internal/modules/mesh_pipeline/*.py")
add_custom_command(OUTPUT ${asset_dir}/low.cpp ${asset_dir}/medium.cpp ${asset_dir}/high.cpp
    ${asset_dir}/low.h ${asset_dir}/medium.h ${asset_dir}/high.h
    BYPRODUCTS ${asset_dir}/low.json ${asset_dir}/medium.json ${asset_dir}/high.json
        ${asset_dir}/low.obj ${asset_dir}/medium.obj ${asset_dir}/high.obj
        ${asset_dir}/low_asset.hpp ${asset_dir}/medium_asset.hpp ${asset_dir}/high_asset.hpp
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/scene3d_assets.py
        --tgx ${PROJECT_SOURCE_DIR}/external/tgx --output ${asset_dir} --benchmark
    DEPENDS ${PROJECT_SOURCE_DIR}/scripts/scene3d_assets.py ${converter_sources}
    VERBATIM COMMENT "Converting shaded 3D benchmark meshes (TGX, no textures)")
add_custom_target(scene3d-assets DEPENDS ${asset_dir}/low.h ${asset_dir}/medium.h ${asset_dir}/high.h)
# Offline-prepared visuals. Blender is only needed to regenerate the source
# OBJ files, not for ordinary builds. These assets are opt-in on hardware.
set(robot_asset_source "${PROJECT_SOURCE_DIR}/src/ui/gui/assets/robot")
set(robot_asset_dir "${asset_dir}/robot")
set(robot_asset_sources "")
foreach(link base_link link_1 link_2 link_3)
    set(quantization_padding 0.0002)
    if(link STREQUAL "link_1")
        # The default quantization lattice oscillates for this mesh. 0.1%
        # padding converges while retaining shared-vertex seam protection.
        set(quantization_padding 0.001)
    endif()
    add_custom_command(OUTPUT ${robot_asset_dir}/robot_${link}.cpp ${robot_asset_dir}/robot_${link}.h
        ${robot_asset_dir}/robot_${link}_asset.hpp
        BYPRODUCTS ${robot_asset_dir}/robot_${link}.json
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/scene3d_assets.py
            --tgx ${PROJECT_SOURCE_DIR}/external/tgx --output ${robot_asset_dir}
            --input ${robot_asset_source}/${link}.obj --name robot_${link}
            --quantization-padding ${quantization_padding}
        DEPENDS ${PROJECT_SOURCE_DIR}/scripts/scene3d_assets.py ${converter_sources}
            ${robot_asset_source}/${link}.obj ${robot_asset_source}/${link}.mtl
        VERBATIM COMMENT "Compiling robot visual ${link} (TGX, no textures)")
    list(APPEND robot_asset_sources ${robot_asset_dir}/robot_${link}.cpp)
endforeach()
add_library(proto_scene3d_robot_assets STATIC EXCLUDE_FROM_ALL ${robot_asset_sources})
add_library(proto::ui::gui::robot_assets ALIAS proto_scene3d_robot_assets)
set_target_properties(proto_scene3d_robot_assets PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_link_libraries(proto_scene3d_robot_assets PUBLIC tgx::engine)
target_include_directories(proto_scene3d_robot_assets PUBLIC ${robot_asset_dir} ${PROJECT_SOURCE_DIR}/src)
add_custom_target(scene3d-robot-assets DEPENDS proto_scene3d_robot_assets)
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
    add_executable(scene3d_robot_tests tests/robot_assets_test.cpp)
    target_link_libraries(scene3d_robot_tests PRIVATE proto::ui::gui::scene3d
        proto::ui::gui::robot_assets GTest::gtest_main)
    runtime_attach(scene3d_robot_tests)
    add_test(NAME gui.scene3d.robot COMMAND scene3d_robot_tests)
    set_tests_properties(gui.scene3d.robot PROPERTIES TIMEOUT 40)
    add_test(NAME gui.scene3d.robot_sources COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${PROJECT_SOURCE_DIR}/scripts/tests/test_robot_visuals.py
        -o cache_dir=${PROJECT_BINARY_DIR}/pytest-cache/robot_visuals)
endif()
