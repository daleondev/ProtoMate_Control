find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(soes_root ${PROJECT_SOURCE_DIR}/external/SOES/soes)
if(NOT EXISTS ${soes_root}/ecat_slv.c)
    message(FATAL_ERROR "Initialize the external/SOES submodule first")
endif()
set(generated ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(
    OUTPUT ${generated}/device.h ${generated}/sii.c ${generated}/ProtoMateEcho.bin
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_sii.py
        ${CMAKE_CURRENT_SOURCE_DIR}/ProtoMateEcho.xml ${generated}
    DEPENDS generate_sii.py ProtoMateEcho.xml
    VERBATIM)
add_library(ethercat_echo STATIC
    ${soes_root}/esc.c ${soes_root}/esc_coe.c ${soes_root}/ecat_slv.c
    objectlist.c soes_port.cpp ${generated}/sii.c ${generated}/device.h)
target_include_directories(ethercat_echo PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} ${generated})
target_include_directories(ethercat_echo SYSTEM PUBLIC ${soes_root} ${soes_root}/include/sys/gcc)
target_link_libraries(ethercat_echo PUBLIC hal::drivers project_compiler_settings)
set_target_properties(ethercat_echo PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
# C stack frames must unwind if a bounded transport operation reports failure
# through our C-linkage C++ callbacks. The top-level worker catches it.
target_compile_options(ethercat_echo PRIVATE -fexceptions)
target_sources(${PROJECT_NAME} PRIVATE main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE ethercat_echo)
