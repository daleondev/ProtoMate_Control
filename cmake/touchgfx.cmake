include_guard(GLOBAL)

set(TOUCHGFX_ROOT "$ENV{TOUCHGFX_ROOT}" CACHE PATH "TouchGFX 4.27.0 directory containing framework/ and lib/")
if(NOT TOUCHGFX_ROOT)
    set(TOUCHGFX_ROOT "${PROJECT_SOURCE_DIR}/external/touchgfx" CACHE PATH "TouchGFX SDK" FORCE)
endif()
if(NOT EXISTS "${TOUCHGFX_ROOT}/framework/include/touchgfx/Version.hpp")
    message(FATAL_ERROR "TouchGFX 4.27.0 is required in external/touchgfx, or set TOUCHGFX_ROOT to another SDK.")
endif()
file(STRINGS "${TOUCHGFX_ROOT}/framework/include/touchgfx/Version.hpp" sdk_version
    REGEX "^#define TOUCHGFX_VERSION +[0-9]+")
if(NOT sdk_version MATCHES "TOUCHGFX_VERSION +42700$")
    message(FATAL_ERROR "Expected TouchGFX 4.27.0, found: ${sdk_version}")
endif()

add_library(touchgfx_engine STATIC IMPORTED GLOBAL)
add_library(touchgfx::engine ALIAS touchgfx_engine)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(engine_library "${TOUCHGFX_ROOT}/lib/linux/libtouchgfx.a")
elseif(STM32_CORE STREQUAL "CM7")
    set(engine_library "${TOUCHGFX_ROOT}/lib/core/cortex_m7/gcc/libtouchgfx-float-abi-hard.a")
else()
    message(FATAL_ERROR "TouchGFX belongs to M7 or Linux, never M4")
endif()
foreach(required_file IN ITEMS "${engine_library}"
        "${TOUCHGFX_ROOT}/framework/tools/fontconvert/build/linux/fontconvert.out"
        "${TOUCHGFX_ROOT}/framework/tools/imageconvert/build/linux/imageconvert.out")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Incomplete TouchGFX SDK: missing ${required_file}")
    endif()
endforeach()
set_target_properties(touchgfx_engine PROPERTIES IMPORTED_LOCATION "${engine_library}"
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${TOUCHGFX_ROOT}/framework/include"
    INTERFACE_INCLUDE_DIRECTORIES "${TOUCHGFX_ROOT}/framework/include")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(RSVG_CONVERT rsvg-convert REQUIRED)
set(touchgfx_assets "${PROJECT_BINARY_DIR}/generated/gui")
file(GLOB_RECURSE gui_image_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/ui/gui/assets/images/*")
file(GLOB_RECURSE touchgfx_converter_sources CONFIGURE_DEPENDS
    "${TOUCHGFX_ROOT}/framework/tools/textconvert/*")
add_custom_command(OUTPUT "${touchgfx_assets}/Assets.cpp"
    COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/scripts/touchgfx_assets.py"
        --sdk "${TOUCHGFX_ROOT}" --source "${PROJECT_SOURCE_DIR}/src/ui/gui/assets"
        --output "${touchgfx_assets}" --gems "${PROJECT_SOURCE_DIR}/apps/ruby-gems"
    DEPENDS "${PROJECT_SOURCE_DIR}/scripts/touchgfx_assets.py" ${gui_image_sources}
        "${PROJECT_SOURCE_DIR}/src/ui/gui/assets/images.json"
        "${PROJECT_SOURCE_DIR}/src/ui/gui/assets/texts.xml"
        "${PROJECT_SOURCE_DIR}/src/ui/gui/assets/fonts/NotoSans-Regular.ttf"
        "${TOUCHGFX_ROOT}/framework/include/touchgfx/Version.hpp"
        "${TOUCHGFX_ROOT}/framework/tools/fontconvert/build/linux/fontconvert.out"
        "${TOUCHGFX_ROOT}/framework/tools/imageconvert/build/linux/imageconvert.out"
        ${touchgfx_converter_sources}
    VERBATIM COMMENT "Generating TouchGFX fonts and text database (Linux converters)")
add_custom_target(gui-assets DEPENDS "${touchgfx_assets}/Assets.cpp")

# Vendor support is compiled independently: no reflection or ThreadX C++ ABI
# overrides, and no project warning policy imposed on the SDK sources.
add_library(touchgfx_support STATIC
    "${touchgfx_assets}/Assets.cpp"
    "${PROJECT_SOURCE_DIR}/src/ui/gui/integration/Paint.cpp"
    "${TOUCHGFX_ROOT}/framework/source/touchgfx/widgets/Box.cpp"
    "${TOUCHGFX_ROOT}/framework/source/touchgfx/widgets/Image.cpp"
    "${TOUCHGFX_ROOT}/framework/source/touchgfx/widgets/TextArea.cpp"
    "${TOUCHGFX_ROOT}/framework/source/touchgfx/widgets/TextAreaWithWildcard.cpp")
add_library(touchgfx::support ALIAS touchgfx_support)
set_target_properties(touchgfx_support PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
target_compile_options(touchgfx_support PRIVATE -w -O2 -ffunction-sections -fdata-sections)
target_link_libraries(touchgfx_support PUBLIC touchgfx::engine)
target_include_directories(touchgfx_support SYSTEM PUBLIC
    "${touchgfx_assets}/images/include" "${touchgfx_assets}/fonts/include" "${touchgfx_assets}/texts/include")
add_dependencies(touchgfx_support gui-assets)

if(STM32_CORE STREQUAL "CM7")
    target_compile_options(touchgfx_support PRIVATE -fno-rtti)
endif()
