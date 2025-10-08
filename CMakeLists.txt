set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -shared -static -Wl,--build-id=none -Wl,-Bsymbolic -Wl,--no-dynamic-linker -Wl,--no-undefined")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffreestanding -nostdlib -mno-outline-atomics -g -fno-common -nostartfiles -mstrict-align -march=armv8-a -Os -std=gnu23 -fno-pic -fno-pie -fno-plt")

# Distinguish windows and linux gcc path
if (WIN32)
    set(CMAKE_MAKE_PROGRAM "E:/JetBrains/CLion 2024.3.3/bin/mingw/bin/mingw32-make.exe")
    set(TARGET_TOOLCHAIN_PREFIX "E:/aarch64-gcc/bin/aarch64-none-elf-")
    set(HOST_C_COMPILER "E:/x86_64-mingw-gcc/bin/x86_64-w64-mingw32-gcc.exe")
    set(CMAKE_C_COMPILER "E:/aarch64-gcc/bin/aarch64-none-elf-gcc.exe")
    set(CMAKE_OBJCOPY "E:/aarch64-gcc/bin/aarch64-none-elf-objcopy.exe")
elseif(UNIX)
    set(HOST_C_COMPILER "/usr/bin/gcc")
#    set(TARGET_TOOLCHAIN_PREFIX "/usr/bin/aarch64-none-elf-")
#    set(CMAKE_C_COMPILER "/usr/bin/aarch64-none-elf-gcc")
#    set(CMAKE_OBJCOPY "/usr/bin/aarch64-none-elf-objcopy")
    set(TARGET_TOOLCHAIN_PREFIX "/usr/bin/aarch64-linux-gnu-")
    set(CMAKE_C_COMPILER "/usr/bin/aarch64-linux-gnu-gcc")
    set(CMAKE_OBJCOPY "/usr/bin/aarch64-linux-gnu-objcopy")
endif()

# If host system is linux and `tput colors` reports less than 256, switch to aquatic style.
set(menuconfig_style "\"path=bg:#51589e,fg:#E6DFF5 list=bg:#B5D7F8,fg:#505E73 separator=bg:#6684DC selection=fg:#1F1A20,bg:#A4C2F2,bold,underline frame=bg:#494f8f body=bg:#6169be help=bg:#51589e,fg:#E6DFF5\"")
if(UNIX)
    find_program(TPUT_EXEC tput)
    if(TPUT_EXEC)
        execute_process(
            COMMAND ${TPUT_EXEC} colors
            OUTPUT_VARIABLE TPUT_COLORS
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(TPUT_COLORS LESS 256)
            message(WARNING "Terminal does not support truecolor.")
            message(WARNING "Please re-run cmake setup in a truecolor terminal.")
            set(menuconfig_style "aquatic")
        endif()
    endif()
endif()


# Output file name
cmake_minimum_required(VERSION 3.10)
set(OUTPUT_NAME kShimC)
set(KSHIMC_VERSION "0.0.1")
project(${OUTPUT_NAME} C ASM)

# Add compile flags for a freestanding environment 

# Add include directories
set(INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/lib/bfdev/include/
    ${CMAKE_BINARY_DIR}/lib/bfdev/generated/
    ${CMAKE_SOURCE_DIR}/src/include/
)

include_directories(${INCLUDE_DIRS})
set(BFDEV_CRC_EXTEND OFF)
include(${CMAKE_SOURCE_DIR}/lib/bfdev/bfdev.cmake)

# Define the executable target, including assembly and C source files
file(GLOB SRC_FILES "src/*.c" "src/lib/*.c" "src/arm64/*.c" "src/arm64/*.S")
include(${CMAKE_SOURCE_DIR}/src/src.cmake)

# Specify the linker script
set(LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/src/arch/arm64/aarch64.lds")
set(PROCESSED_LINKER_SCRIPT "${CMAKE_BINARY_DIR}/aarch64.lds")
add_custom_command(
    OUTPUT ${PROCESSED_LINKER_SCRIPT}
    COMMAND ${CMAKE_C_COMPILER} -E -P -x c ${LINKER_SCRIPT} -o ${PROCESSED_LINKER_SCRIPT} -I${CMAKE_SOURCE_DIR}/src/include/ -D__LD_SCRIPT__
    DEPENDS ${LINKER_SCRIPT} ${CMAKE_SOURCE_DIR}/src/include/ld/*
    COMMENT "Preprocessing linker script"
)

add_custom_target(preprocess_linker_script ALL DEPENDS ${PROCESSED_LINKER_SCRIPT})
add_executable(${OUTPUT_NAME}.elf ${SRC_FILES})
add_dependencies(${OUTPUT_NAME}.elf preprocess_linker_script)
target_link_libraries(${OUTPUT_NAME}.elf bfdev)
target_link_libraries(${OUTPUT_NAME}.elf gcc)
set_target_properties(${OUTPUT_NAME}.elf PROPERTIES LINK_FLAGS "-T${PROCESSED_LINKER_SCRIPT}")

# After building the ELF file, generate a binary file (main.bin) using objcopy
add_custom_command(TARGET ${OUTPUT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary ${OUTPUT_NAME}.elf ${OUTPUT_NAME}.bin
    COMMENT "Generating executable binary"
)

# Generate common variables pass to Kconfig
set(HOST_ARCH "${CMAKE_HOST_SYSTEM_PROCESSOR}")
string(TIMESTAMP BUILD_TIME "%Y-%m-%d %H:%M:%S" UTC)
get_filename_component(COMPILER_BASENAME "${CMAKE_C_COMPILER}" NAME)
set(HOST_INFO "$ENV{USER}$ENV{USERNAME}@$ENV{HOSTNAME}$ENV{COMPUTERNAME} ${COMPILER_BASENAME} (${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION})")
set(KCONFIG_ENV "srctree=${CMAKE_SOURCE_DIR}/src" "HOST_ARCH=${HOST_ARCH}" "HOST_INFO=${HOST_INFO}"
                "BUILD_TIME=${BUILD_TIME}" "ARCH=${CMAKE_SYSTEM_PROCESSOR}" "VERSION=${KSHIMC_VERSION}"
)

# Add a custom target for menuconfig
add_custom_target(menuconfig
    COMMAND ${CMAKE_COMMAND} -E env "MENUCONFIG_STYLE=${menuconfig_style}" ${KCONFIG_ENV} menuconfig "${CMAKE_SOURCE_DIR}/Kconfig"
    COMMENT "Starting Menu Config"
)

# Add a custom target for guionfig
add_custom_target(guiconfig
    COMMAND ${CMAKE_COMMAND} -E echo "Running guiconfig..."
    COMMAND ${CMAKE_COMMAND} -E env ${KCONFIG_ENV} guiconfig "${CMAKE_SOURCE_DIR}/Kconfig"
    COMMENT "Starting GUI Config"
)

# Generate the configuration header file after menuconfig/guiconfig/
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)
add_custom_command(
    TARGET menuconfig guiconfig
    COMMAND ${CMAKE_COMMAND} -E env "srctree=$(CMAKE_SOURCE_DIR)/src" genconfig "${CMAKE_SOURCE_DIR}/Kconfig" "--header-path" "${CMAKE_BINARY_DIR}/generated/config.h"
    DEPENDS menuconfig guiconfig ${CMAKE_BINARY_DIR}/.config
    COMMENT "Generating configuration header file"
)