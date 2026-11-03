cmake_minimum_required(VERSION 3.20)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -shared -static -Wl,--build-id=none -Wl,-Bsymbolic -Wl,--no-dynamic-linker -Wl,--no-undefined -Wl,--gc-sections")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffreestanding -nostdlib -mno-outline-atomics -g -fno-common -nostartfiles -mstrict-align -march=armv8-a -Os -std=gnu2x -fno-pic -fno-pie -fno-plt -ffunction-sections -fdata-sections -Wall")

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
set(OUTPUT_NAME kShimC)
set(KSHIMC_VERSION "0.0.1")
project(${OUTPUT_NAME} C ASM)

option(KSHIM_STANDALONE "Boot directly as an ARM64 Image instead of an attached manifest runtime" OFF)
set(KSHIM_LINK_ADDRESS "0x40200000" CACHE STRING "Runtime link address")
set(KSHIM_RUNTIME_SLOT_SIZE "0x400000" CACHE STRING "Reserved runtime bytes (4 MiB for manifest protocol)")
set(KSHIM_UEFI_PAYLOAD "" CACHE FILEPATH "Optional raw UEFI FD for standalone test menu")
add_compile_definitions(KSHIM_STANDALONE=$<BOOL:${KSHIM_STANDALONE}>
    KSHIM_LINK_ADDRESS=${KSHIM_LINK_ADDRESS} KSHIM_RUNTIME_SLOT_SIZE=${KSHIM_RUNTIME_SLOT_SIZE})

# Generate common variables pass to Kconfig
set(HOST_ARCH "${CMAKE_HOST_SYSTEM_PROCESSOR}")
string(TIMESTAMP BUILD_TIME "%Y-%m-%d %H:%M:%S" UTC)
get_filename_component(COMPILER_BASENAME "${CMAKE_C_COMPILER}" NAME)
set(HOST_INFO "$ENV{USER}$ENV{USERNAME}@$ENV{HOSTNAME}$ENV{COMPUTERNAME} ${COMPILER_BASENAME} (${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION})")
set(KCONFIG_ENV "srctree=${CMAKE_SOURCE_DIR}" "HOST_ARCH=${HOST_ARCH}" "HOST_INFO=${HOST_INFO}"
                "BUILD_TIME=${BUILD_TIME}" "ARCH=${CMAKE_SYSTEM_PROCESSOR}" "VERSION=${KSHIMC_VERSION}"
)

find_program(MENUCONFIG_EXECUTABLE menuconfig)
find_program(GUICONFIG_EXECUTABLE guiconfig)
find_program(GENCONFIG_EXECUTABLE genconfig)

if(NOT GENCONFIG_EXECUTABLE)
    message(FATAL_ERROR "genconfig (Python kconfiglib) is required")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_BINARY_DIR}/.config" "${CMAKE_SOURCE_DIR}/Kconfig"
    "${CMAKE_SOURCE_DIR}/kernel/Kconfig" "${CMAKE_SOURCE_DIR}/kernel/drivers/Kconfig"
    "${CMAKE_SOURCE_DIR}/kernel/drivers/irqchip/Kconfig"
    "${CMAKE_SOURCE_DIR}/user/Kconfig"
    "${CMAKE_SOURCE_DIR}/tools/configure_kconfig.py")
execute_process(COMMAND ${CMAKE_COMMAND} -E env ${KCONFIG_ENV}
    ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/configure_kconfig.py
    --root ${CMAKE_SOURCE_DIR} --config ${CMAKE_BINARY_DIR}/.config
    --header ${CMAKE_BINARY_DIR}/generated/include/config.h
    --cmake ${CMAKE_BINARY_DIR}/generated/config.cmake
    RESULT_VARIABLE CONFIG_STATUS)
if(NOT CONFIG_STATUS EQUAL 0)
    message(FATAL_ERROR "Kconfig evaluation failed")
endif()
include(${CMAKE_BINARY_DIR}/generated/config.cmake)
add_custom_target(generate_config)

# Build Picolibc for the existing kShimC runtime and startup environment.
set(__THREAD_LOCAL_STORAGE OFF CACHE BOOL "" FORCE)
set(__THREAD_LOCAL_STORAGE_API OFF CACHE BOOL "" FORCE)
set(__GLOBAL_ERRNO ON CACHE BOOL "" FORCE)
set(__SINGLE_THREAD ON CACHE BOOL "" FORCE)
set(ENABLE_MALLOC ON CACHE BOOL "" FORCE)
set(POSIX_CONSOLE OFF CACHE BOOL "" FORCE)
set(__IO_FLOAT OFF CACHE BOOL "" FORCE)
set(__IO_DEFAULT l CACHE STRING "" FORCE)
set(__IO_LONG_DOUBLE OFF CACHE BOOL "" FORCE)
set(__IO_POS_ARGS OFF CACHE BOOL "" FORCE)
set(__MB_CAPABLE OFF CACHE BOOL "" FORCE)
set(TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/picolibc ${CMAKE_BINARY_DIR}/lib/picolibc)

# Add compile flags for a freestanding environment 

# Include directories shared by both sides: the generated Kconfig output and
# the public kernel API (include/kshim/). kernel/include and user/include are
# private to their side; see docs/source-layout.md.
include_directories(
    ${CMAKE_BINARY_DIR}/generated/
    ${CMAKE_BINARY_DIR}/generated/include/
    ${CMAKE_SOURCE_DIR}/include/
)
set(KERNEL_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/kernel/include/
    ${CMAKE_SOURCE_DIR}/lib/libfdt/include/
)
set(USER_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/user/include/
)

# Kernel: arch, core, DT, drivers, libc port and libfdt.
set(KERNEL_SOURCES
    kernel/core/power.c kernel/drivers/power.c kernel/dt/power.c kernel/dt/spmi.c
    kernel/core/main.c kernel/core/platform.c kernel/core/uefi_boot.c kernel/lib/picoport.c
    kernel/core/input.c kernel/core/display.c
    kernel/dt/dt.c kernel/dt/console.c kernel/dt/resources.c
    kernel/drivers/console.c kernel/drivers/pl011.c kernel/drivers/keys.c
    kernel/arch/arm64/exception.c kernel/arch/arm64/relocation.c kernel/arch/arm64/mmu.c
    kernel/arch/arm64/psci.c kernel/arch/arm64/timer.c kernel/arch/arm64/smp.c kernel/arch/arm64/smp_selftest.c kernel/arch/arm64/gicv3.c
    kernel/arch/arm64/startup.S kernel/arch/arm64/mpstartup.S kernel/arch/arm64/arm64.S
    kernel/arch/arm64/psci.S kernel/arch/arm64/timer.S kernel/arch/arm64/smc.S kernel/arch/arm64/gicv3.S
    lib/libfdt/fdt.c lib/libfdt/fdt_check.c lib/libfdt/fdt_ro.c lib/libfdt/fdt_addresses.c
    lib/libfdt/fdt_rw.c lib/libfdt/fdt_sw.c lib/libfdt/fdt_wip.c lib/libfdt/fdt_empty_tree.c lib/libfdt/fdt_strerror.c)
# Userspace: boot-entry selection and the LVGL boot menu.
set(USER_SOURCES user/bootmenu/bootmenu.c)

if(CONFIG_QCOM_KEYS OR CONFIG_KSHIM_TOUCH OR CONFIG_QCOM_IRQCHIP)
    list(APPEND KERNEL_SOURCES kernel/dt/gpio.c)
endif()
# Qualcomm interrupt cascades and interrupt-driven drivers (docs/interrupts.md).
if(CONFIG_QCOM_IRQCHIP)
    list(APPEND KERNEL_SOURCES kernel/dt/qcom_irq.c kernel/drivers/irqchip/resolve.c
        kernel/drivers/irqchip/tlmm.c kernel/drivers/irqchip/pdc.c kernel/drivers/irqchip/handoff.c)
    if(CONFIG_QCOM_SPMI)
        list(APPEND KERNEL_SOURCES kernel/drivers/irqchip/spmi_pmic.c)
    endif()
endif()
if(CONFIG_KSHIM_INPUT_IRQ)
    list(APPEND KERNEL_SOURCES kernel/drivers/input_irq.c)
endif()
if(CONFIG_KSHIM_CONSOLE_RX_IRQ)
    list(APPEND KERNEL_SOURCES kernel/drivers/geni_rx_irq.c)
endif()
if(CONFIG_QCOM_KEYS)
    list(APPEND KERNEL_SOURCES kernel/dt/keys.c)
endif()
if(CONFIG_QCOM_KEYS OR CONFIG_QCOM_SPMI)
    list(APPEND KERNEL_SOURCES kernel/drivers/qcom_keys.c)
endif()
if(CONFIG_KSHIM_TOUCH)
    list(APPEND KERNEL_SOURCES kernel/dt/touch.c kernel/drivers/st_ftm5.c)
endif()
if(CONFIG_KSHIM_FRAMEBUFFER)
    list(APPEND KERNEL_SOURCES kernel/dt/splash.c)
endif()
if(CONFIG_KSHIM_FRAMEBUFFER OR CONFIG_KSHIM_LVGL)
    list(APPEND USER_SOURCES user/ui/framebuffer.c)
endif()
if(CONFIG_KSHIM_TOUCH OR CONFIG_KSHIM_LVGL)
    list(APPEND KERNEL_SOURCES kernel/drivers/touch.c)
endif()
if(CONFIG_KSHIM_LVGL)
    list(APPEND USER_SOURCES user/ui/backdrop.c user/ui/font.c user/ui/lvgl_port.c
        user/lvgl/lvgl_os.c user/lvgl/lvgl_selftest.c)
    set(KSHIM_UI_FONT_BLOB "${CMAKE_SOURCE_DIR}/user/ui/fonts/noto_sans_cjk_sc.ksf")
    configure_file(user/ui/fonts/ui_font_blob.S.in ${CMAKE_BINARY_DIR}/ui_font_blob.S @ONLY)
    set_source_files_properties(${CMAKE_BINARY_DIR}/ui_font_blob.S PROPERTIES
        OBJECT_DEPENDS "${KSHIM_UI_FONT_BLOB}")
    list(APPEND USER_SOURCES ${CMAKE_BINARY_DIR}/ui_font_blob.S)
endif()
if(KSHIM_UEFI_PAYLOAD)
    get_filename_component(KSHIM_UEFI_PAYLOAD "${KSHIM_UEFI_PAYLOAD}" ABSOLUTE)
    configure_file(kernel/arch/arm64/uefi_payload.S.in ${CMAKE_BINARY_DIR}/uefi_payload.S @ONLY)
    set_source_files_properties(${CMAKE_BINARY_DIR}/uefi_payload.S PROPERTIES OBJECT_DEPENDS "${KSHIM_UEFI_PAYLOAD}")
    list(APPEND KERNEL_SOURCES ${CMAKE_BINARY_DIR}/uefi_payload.S)
endif()
set(CRDK_SPMI ${CONFIG_QCOM_SPMI} CACHE BOOL "" FORCE)
if(CONFIG_QCOM_KEYS OR CONFIG_BLK_SDHCI OR CONFIG_QCOM_IRQCHIP)
    set(CRDK_GPIO ON CACHE BOOL "" FORCE)
else()
    set(CRDK_GPIO OFF CACHE BOOL "" FORCE)
endif()
# Storage votes rails through the polled CMD DB/RPMh API and uses ClockLib.
if(CONFIG_KSHIM_TOUCH OR CONFIG_BLK_UFS OR CONFIG_BLK_SDHCI)
    set(CRDK_GENI ON CACHE BOOL "" FORCE)
else()
    set(CRDK_GENI OFF CACHE BOOL "" FORCE)
endif()
set(CRDK_DISPLAY ${CONFIG_KSHIM_FRAMEBUFFER} CACHE BOOL "" FORCE)
set(CRDK_IRQCHIP ${CONFIG_QCOM_IRQCHIP} CACHE BOOL "" FORCE)
# Console type is selected from DT, so UART support remains a capability in every profile.
set(CRDK_UART ON CACHE BOOL "" FORCE)

if(CONFIG_KSHIM_LVGL)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
set(LV_CONF_PATH "${CMAKE_SOURCE_DIR}/user/include/lv_conf.h" CACHE FILEPATH "" FORCE)
add_subdirectory(third_party/lvgl EXCLUDE_FROM_ALL)
include(${CMAKE_SOURCE_DIR}/ports/lvgl/CMakeLists.txt)
kshim_enable_lvgl_smp_adapter(lvgl "${CMAKE_SOURCE_DIR}/third_party/lvgl")
target_link_libraries(lvgl PRIVATE c)

set(KSHIM_UI_LIBRARIES lvgl)
endif()
add_subdirectory(ports/crdk)
target_link_libraries(crdk PRIVATE c)
include(${CMAKE_SOURCE_DIR}/cmake/storage.cmake)
include(kernel/drivers/block/block.cmake)

# Specify the linker script
set(LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/kernel/arch/arm64/aarch64.lds")
set(PROCESSED_LINKER_SCRIPT "${CMAKE_BINARY_DIR}/aarch64.lds")
file(GLOB LINKER_HEADERS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/kernel/include/ld/*")
add_custom_command(
    OUTPUT ${PROCESSED_LINKER_SCRIPT}
    COMMAND ${CMAKE_C_COMPILER} -E -P -x c ${LINKER_SCRIPT} -o ${PROCESSED_LINKER_SCRIPT} -I${CMAKE_SOURCE_DIR}/kernel/include/ -D__LD_SCRIPT__ -DKSHIM_LINK_ADDRESS=${KSHIM_LINK_ADDRESS} -DKSHIM_RUNTIME_SLOT_SIZE=${KSHIM_RUNTIME_SLOT_SIZE}
    DEPENDS ${LINKER_SCRIPT} ${LINKER_HEADERS}
    COMMENT "Preprocessing linker script"
)

add_custom_target(preprocess_linker_script ALL DEPENDS ${PROCESSED_LINKER_SCRIPT})

# Kernel and userspace compile separately so each side only sees its own
# private headers plus the public include/kshim API.
add_library(kshim_kernel OBJECT ${KERNEL_SOURCES})
target_include_directories(kshim_kernel PRIVATE ${KERNEL_INCLUDE_DIRS})
target_link_libraries(kshim_kernel PRIVATE crdk c)
if(KSHIM_UEFI_PAYLOAD)
    target_compile_definitions(kshim_kernel PRIVATE KSHIM_HAS_UEFI_PAYLOAD=1)
endif()
add_library(kshim_user OBJECT ${USER_SOURCES})
target_include_directories(kshim_user PRIVATE ${USER_INCLUDE_DIRS})
target_link_libraries(kshim_user PRIVATE ${KSHIM_UI_LIBRARIES} c)

add_executable(${OUTPUT_NAME}.elf
    $<TARGET_OBJECTS:kshim_kernel> $<TARGET_OBJECTS:kshim_user>)
add_dependencies(${OUTPUT_NAME}.elf preprocess_linker_script)
target_link_libraries(${OUTPUT_NAME}.elf PRIVATE
    "-Wl,--start-group"
    ${KSHIM_UI_LIBRARIES}
    ${KSHIM_STORAGE_LIBRARIES}
    ${KSHIM_BLOCK_LIBRARIES}
    crdk
    c
    gcc
    "-Wl,--end-group"
)
set_target_properties(${OUTPUT_NAME}.elf PROPERTIES LINK_FLAGS "-T${PROCESSED_LINKER_SCRIPT}")

# After building the ELF file, generate a binary file (main.bin) using objcopy
add_custom_command(TARGET ${OUTPUT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary ${OUTPUT_NAME}.elf ${OUTPUT_NAME}.bin
    COMMENT "Generating executable binary"
)

if(MENUCONFIG_EXECUTABLE AND GENCONFIG_EXECUTABLE)
    add_custom_target(menuconfig
        COMMAND ${CMAKE_COMMAND} -E env "MENUCONFIG_STYLE=${menuconfig_style}" ${KCONFIG_ENV} ${MENUCONFIG_EXECUTABLE} "${CMAKE_SOURCE_DIR}/Kconfig"
        COMMAND ${CMAKE_COMMAND} -E env ${KCONFIG_ENV} ${GENCONFIG_EXECUTABLE} "${CMAKE_SOURCE_DIR}/Kconfig" "--header-path" "${CMAKE_BINARY_DIR}/generated/include/config.h"
        COMMENT "Starting Menu Config"
    )
endif()

if(GUICONFIG_EXECUTABLE AND GENCONFIG_EXECUTABLE)
    add_custom_target(guiconfig
        COMMAND ${CMAKE_COMMAND} -E env ${KCONFIG_ENV} ${GUICONFIG_EXECUTABLE} "${CMAKE_SOURCE_DIR}/Kconfig"
        COMMAND ${CMAKE_COMMAND} -E env ${KCONFIG_ENV} ${GENCONFIG_EXECUTABLE} "${CMAKE_SOURCE_DIR}/Kconfig" "--header-path" "${CMAKE_BINARY_DIR}/generated/include/config.h"
        COMMENT "Starting GUI Config"
    )
endif()
