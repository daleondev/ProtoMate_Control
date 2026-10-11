# Use unsigned bitmap masks: signed 1 << 31 is undefined in C.
# Patch build-directory copies, leaving the pinned LevelX sources untouched.
function(runtime_patch_levelx)
    find_program(PATCH_EXECUTABLE NAMES patch REQUIRED)
    set(patched_sources)
    set(patches
        "lx_nor_flash_extended_cache_enable.c|0ac4316b8a145a7d81d48bd95216a0391ee5aa9ad2821004b7721df1c0f89b53|7059ae5434c85009664bb81bc2841bff94c7bfde7eb602d6be53d40b8c2c9c7a"
        "lx_nor_flash_logical_sector_find.c|412ff140e4cdf7cbbe2ede80a4f7208ecac7f57545dc2bc04d0676e37b924c6c|f2f7677e9343967d086a3190c64ebb8ded5bbb4edb8d87a6f44d6dda32ce72e9"
        "lx_nor_flash_open_extended.c|18bb23ee1cd3126c2ff97a4bffc6d2420962918a1ed1b25982100d6353ffc215|5cfe516403854aeb829f5164666c89befa4e1a7828e0fdb4dc7a50d1aefc9a78"
        "lx_nor_flash_sector_read.c|a483a9c023fb916ba6c9e62aa40c9d5e36ada09a956b642351e7e446d761e2bb|8d85e16639becf9706d469ca1cc60af8c01e4e65ba413b799aa144f0162a7614"
        "lx_nor_flash_sector_release.c|bf933b825d3612422a07eefff1d10a0f530fa0679eb37d5bf965103aaf37ddfb|b6b51f409512fdc0b3dfe85f9e5de2e5647c6bb780104ea4bad165fbd17f81f1"
        "lx_nor_flash_sector_write.c|b2b1b609dec901cfc4c650c099af9a2f02a10ae58e1af6c938138d9526158d49|e848763c878feb22a32790e5e68cee846d98640a36714b6ec4a2b242087267b9"
    )
    file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/levelx-patched")
    get_target_property(levelx_sources levelx SOURCES)
    foreach(entry IN LISTS patches)
        string(REPLACE "|" ";" fields "${entry}")
        list(GET fields 0 filename)
        list(GET fields 1 original_hash)
        list(GET fields 2 fixed_hash)
        set(source "${PROJECT_SOURCE_DIR}/external/levelx/common/src/${filename}")
        set(patch_file "${PROJECT_SOURCE_DIR}/platform/runtime/libc/patches/${filename}.patch")
        set(output "${PROJECT_BINARY_DIR}/levelx-patched/${filename}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}" "${patch_file}")
        file(SHA256 "${source}" source_hash)
        if(source_hash STREQUAL original_hash)
            execute_process(COMMAND "${PATCH_EXECUTABLE}" --batch --fuzz=0 --silent
                            --output "${output}" "${source}" "${patch_file}"
                            RESULT_VARIABLE patch_result)
            if(NOT patch_result EQUAL 0)
                message(FATAL_ERROR "LevelX bitmap patch failed for ${filename}: ${patch_result}")
            endif()
        elseif(source_hash STREQUAL fixed_hash)
            configure_file("${source}" "${output}" COPYONLY)
        else()
            message(FATAL_ERROR "Unexpected LevelX ${filename}; review dependency changes before updating its patch")
        endif()
        file(SHA256 "${output}" output_hash)
        if(NOT output_hash STREQUAL fixed_hash)
            message(FATAL_ERROR "LevelX patched ${filename} checksum does not match the reviewed fix")
        endif()
        list(REMOVE_ITEM levelx_sources "${source}")
        list(APPEND patched_sources "${output}")
    endforeach()
    set_property(TARGET levelx PROPERTY SOURCES "${levelx_sources};${patched_sources}")
endfunction()
