set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TOOLCHAIN_PREFIX arm-none-eabi)

set(CMAKE_C_COMPILER "${TOOLCHAIN_PREFIX}-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PREFIX}-g++")
set(CMAKE_ASM_COMPILER "${TOOLCHAIN_PREFIX}-gcc")
set(CMAKE_AR "${TOOLCHAIN_PREFIX}-ar")
set(CMAKE_LINKER "${TOOLCHAIN_PREFIX}-ld")
set(CMAKE_OBJCOPY "${TOOLCHAIN_PREFIX}-objcopy")
set(CMAKE_RANLIB "${TOOLCHAIN_PREFIX}-ranlib")
set(CMAKE_SIZE "${TOOLCHAIN_PREFIX}-size")
set(CMAKE_STRIP "${TOOLCHAIN_PREFIX}-strip")

set(CMAKE_EXECUTABLE_SUFFIX_ASM ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_C ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_CXX ".elf")

# Compiler checks cannot link firmware without the application linker script.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(ARM_CPU_FLAGS "-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard")

# Toolchain files are evaluated again in CMake's try_compile projects. Use the
# *_INIT variables so flags are initialized once instead of repeatedly appended.
set(CMAKE_C_FLAGS_INIT "${ARM_CPU_FLAGS} -fdata-sections -ffunction-sections")
set(CMAKE_CXX_FLAGS_INIT "${ARM_CPU_FLAGS} -fdata-sections -ffunction-sections -Wno-psabi")
set(CMAKE_ASM_FLAGS_INIT "${ARM_CPU_FLAGS} -x assembler-with-cpp")

set(CMAKE_C_FLAGS_DEBUG "-O0 -g3" CACHE STRING "C debug flags")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g3" CACHE STRING "C++ debug flags")
set(CMAKE_ASM_FLAGS_DEBUG "-g3" CACHE STRING "ASM debug flags")

set(CMAKE_C_FLAGS_RELEASE "-Os -g0 -DNDEBUG" CACHE STRING "C release flags")
set(CMAKE_CXX_FLAGS_RELEASE "-Os -g0 -DNDEBUG" CACHE STRING "C++ release flags")
set(CMAKE_ASM_FLAGS_RELEASE "-g0" CACHE STRING "ASM release flags")

# Search host tools on the host and headers/libraries/packages in the target
# toolchain sysroot inferred by the Arm compiler.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
