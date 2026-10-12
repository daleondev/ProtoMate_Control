# Report the linked image on every default build without forcing a relink.
function(firmware_memory_usage target map_file)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_target(memory-usage ALL
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/memory_usage.py
            --map ${map_file} --elf $<TARGET_FILE:${target}>
            --objdump ${CMAKE_OBJDUMP}
        DEPENDS ${target}
        COMMENT "Memory usage"
        VERBATIM USES_TERMINAL)
endfunction()
