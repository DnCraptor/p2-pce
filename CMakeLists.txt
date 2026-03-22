# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

# Set board to Pico 2 (RP2350)
set(PICO_BOARD pico2 CACHE STRING "Pico board type")

set(BUILD_NAME "pce")

# Board configuration variant (M1, M2, PC or Z2)
set(BOARD "M1" CACHE STRING "Board variant: M1, M2, PC or Z2")
# Alias for backwards compatibility
if(DEFINED BOARD_VARIANT AND NOT DEFINED BOARD)
    set(BOARD ${BOARD_VARIANT})
endif()

# CPU overclock configuration (252, 378, 504 MHz)
set(CPU_SPEED "504" CACHE STRING "CPU clock in MHz: 378, 504")

# PSRAM speed configuration
set(PSRAM_SPEED "66" CACHE STRING "PSRAM max frequency in MHz: 66, 84, 100, 133, 166")

# Audio output configuration (I2S or PWM)
set(AUDIO_TYPE "PWM" CACHE STRING "Audio output: I2S or PWM")
# DVI audio
set(DVI_A OFF)

# Enable LTEMM support
option(EMM "Enable LTEMM support" OFF)

# USB HID keyboard support
option(USB_HID_ENABLED "Enable USB HID keyboard (disables USB CDC stdio)" ON)

# Debug output
option(DEBUG_ENABLED "Enable verbose debug output" OFF)

# Profiling
option(PROFILE_ENABLED "Enable instruction profiling" OFF)

if(BOARD STREQUAL "M1")
    SET(BUILD_NAME "m1p2-${BUILD_NAME}")
elseif(BOARD STREQUAL "PC")
    SET(BUILD_NAME "PCp2-${BUILD_NAME}")
elseif(BOARD STREQUAL "Z2")
    SET(BUILD_NAME "z0p2-${BUILD_NAME}")
else()  # M2
    SET(BUILD_NAME "m2p2-${BUILD_NAME}")
endif()

# CPU voltage selection based on speed
if(CPU_SPEED GREATER_EQUAL 504)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_65")
elseif(CPU_SPEED GREATER_EQUAL 378)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_50")
endif()

message(STATUS "pce: Board=${BOARD}, CPU=${CPU_SPEED}MHz, PSRAM=${PSRAM_SPEED}MHz, Voltage=${CPU_VOLTAGE} ${AUDIO_TYPE}")

include(pico_sdk_import.cmake)

set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${OUTPUT_DIR}")

project(${BUILD_NAME} C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)  # gnu11 вместо c11 — нужно для newlib POSIX-функций
set(CMAKE_CXX_STANDARD 17)

# Read version from version file
file(READ "${CMAKE_CURRENT_LIST_DIR}/version" VERSION_CONTENT)
string(STRIP "${VERSION_CONTENT}" VERSION_CONTENT)
string(REPLACE " " ";" VERSION_LIST "${VERSION_CONTENT}")
list(GET VERSION_LIST 0 PCE_VERSION_MAJOR)
list(GET VERSION_LIST 1 PCE_VERSION_MINOR)
list(GET VERSION_LIST 2 PCE_VERSION_ITERATION)
set(PCE_VERSION_STR "${PCE_VERSION_MAJOR}.${PCE_VERSION_MINOR}.${PCE_VERSION_ITERATION}")
message(STATUS "pce version: ${PCE_VERSION_STR}")

pico_sdk_init()

#=============================================================================
# FatFS Library
#=============================================================================
add_library(fatfs
    drivers/fatfs/ff.c
    drivers/fatfs/ffsystem.c
    drivers/fatfs/ffunicode.c
    drivers/fatfs/f_util.c
)
target_include_directories(fatfs PUBLIC drivers/fatfs)
target_link_libraries(fatfs pico_stdlib)

#=============================================================================
# SD Card Driver
#=============================================================================
add_library(sdcard
    drivers/sdcard/sdcard.c
    drivers/sdcard/pio_spi.c
)
target_include_directories(sdcard PUBLIC drivers/sdcard drivers/fatfs src)
target_compile_definitions(sdcard PRIVATE
    BOARD_${BOARD}
    RP2350_BUILD=1
)
target_link_libraries(sdcard pico_stdlib hardware_spi hardware_pio hardware_dma)
pico_generate_pio_header(sdcard ${CMAKE_CURRENT_LIST_DIR}/drivers/sdcard/spi.pio)

#=============================================================================
# PS/2 Keyboard Driver
#=============================================================================
add_library(ps2kbd
    drivers/ps2kbd/ps2kbd_mrmltr.cpp
    drivers/ps2kbd/ps2kbd_wrapper.cpp
)
target_include_directories(ps2kbd PUBLIC drivers/ps2kbd src)
target_compile_definitions(ps2kbd PRIVATE BOARD_${BOARD})
target_link_libraries(ps2kbd pico_stdlib hardware_pio)
pico_generate_pio_header(ps2kbd ${CMAKE_CURRENT_LIST_DIR}/drivers/ps2kbd/ps2kbd_mrmltr.pio)
pico_generate_pio_header(ps2kbd ${CMAKE_CURRENT_LIST_DIR}/drivers/ps2kbd/ps2kbd_mrmltr2.pio)

#=============================================================================
# PS/2 Mouse Driver
#=============================================================================
add_library(ps2mouse
    drivers/ps2mouse/ps2mouse.c
)
target_include_directories(ps2mouse PUBLIC drivers/ps2mouse src)
target_compile_definitions(ps2mouse PRIVATE BOARD_${BOARD})
target_link_libraries(ps2mouse pico_stdlib hardware_gpio)

#=============================================================================
# PSRAM Driver
#=============================================================================
add_library(psram
    drivers/psram/psram_init.c
)
target_include_directories(psram PUBLIC drivers/psram src)
target_compile_definitions(psram PRIVATE
    BOARD_${BOARD}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
)
target_link_libraries(psram pico_stdlib hardware_clocks)

#=============================================================================
# VGA Video Driver
#=============================================================================
if (DVI_A)
    add_library(video_driver
        drivers/vga/test_pins.c
        drivers/vga/vga_hw.c
        drivers/vga/vga_osd.c
        drivers/vga/font8x16.c
    )
else()
    add_library(video_driver
        drivers/hdmi/hdmi.c
        drivers/vga/test_pins.c
        drivers/vga/vga_hw.c
        drivers/vga/vga_osd.c
        drivers/vga/font8x16.c
    )
endif()
target_include_directories(video_driver PUBLIC drivers/vga drivers/hdmi src)
target_compile_definitions(video_driver PRIVATE BOARD_${BOARD} RP2350_BUILD=1)
target_link_libraries(video_driver pico_stdlib hardware_dma hardware_pio hardware_clocks hardware_sync)

#=============================================================================
# I2S/PWM Audio Driver
#=============================================================================
add_library(audio_driver
    drivers/audio/audio.c
)
target_include_directories(audio_driver PUBLIC drivers/audio src)
target_compile_definitions(audio_driver PRIVATE BOARD_${BOARD})
pico_generate_pio_header(audio_driver ${CMAKE_CURRENT_LIST_DIR}/drivers/audio/audio_i2s.pio)
if(AUDIO_TYPE STREQUAL "I2S")
    target_link_libraries(audio_driver pico_stdlib hardware_dma hardware_pio hardware_clocks hardware_sync hardware_irq)
    target_compile_definitions(audio_driver PRIVATE FEATURE_AUDIO_I2S=1 SOUND_FREQUENCY=44100)
elseif(AUDIO_TYPE STREQUAL "PWM")
    target_link_libraries(audio_driver pico_stdlib hardware_dma hardware_pio hardware_clocks hardware_sync hardware_irq hardware_pwm)
    target_compile_definitions(audio_driver PRIVATE FEATURE_AUDIO_PWM=1 SOUND_FREQUENCY=44100)
endif()

if (DVI_A)
    add_subdirectory(drivers/libdvi)
endif()

#=============================================================================
# USB HID Keyboard Driver (optional)
#=============================================================================
add_subdirectory(drivers/usbhid)

if(EMM)
    SET(BUILD_NAME "${BUILD_NAME}-${CPU_SPEED}MHz-P${PSRAM_SPEED}-${AUDIO_TYPE}-${PCE_VERSION_STR}-emm")
else ()
    SET(BUILD_NAME "${BUILD_NAME}-${CPU_SPEED}MHz-P${PSRAM_SPEED}-${AUDIO_TYPE}-${PCE_VERSION_STR}")
endif()

#=============================================================================
# Core Emulator Sources
#=============================================================================
set(EMU_CORE_SOURCES
    # Mac arch
    src/arch/macplus/macplus.c
    src/arch/macplus/adb.c
    src/arch/macplus/adb_keyboard.c
    src/arch/macplus/adb_mouse.c
    src/arch/macplus/cmd_68k.c
    src/arch/macplus/hook.c
    src/arch/macplus/hotkey.c
    src/arch/macplus/iwm.c
    src/arch/macplus/iwm-io.c
    src/arch/macplus/keyboard.c
    src/arch/macplus/mem.c
    src/arch/macplus/msg.c
    src/arch/macplus/rtc.c
    src/arch/macplus/scsi.c
    src/arch/macplus/serial.c
    src/arch/macplus/sony.c
    src/arch/macplus/sound.c
    src/arch/macplus/traps.c
    src/arch/macplus/video.c

    # PCE chipset + CPU
    src/chipset/e8530.c
    src/chipset/e6522.c
    src/cpu/e68000/e68000.c
    src/cpu/e68000/opcodes.c     # e68_set_opcodes
    src/cpu/e68000/cc.c          # e68_cc_set_*
    src/cpu/e68000/ea.c

    # PCE devices
    src/devices/memory.c
    src/devices/nvram.c

    # PCE lib
    # log/sysdep/console/path заменены rp2350_stubs.c — НЕ включать их сюда
    src/lib/brkpt.c
    src/lib/inidsk.c
    src/lib/iniram.c
    src/lib/load.c
    src/lib/ihex.c               # ihex_load_fp (из load.c)
    src/lib/srec.c               # srec_load_fp (из load.c)
    src/lib/msg.c                # msg_get_uint/bool/sint
    src/lib/monitor.c            # mon_set_terminate
    src/lib/string.c             # str_cat_alloc
    src/lib/initerm.c            # ini_get_terminal -> возвращает NULL (trm=NULL headless)

    # PCE drivers
    src/drivers/options.c        # drv_get_option*
    src/drivers/video/keys.c
    src/drivers/video/terminal.c
    src/drivers/video/null.c
    src/drivers/block/block.c
    src/drivers/block/blkcow.c
    src/drivers/block/blkfdc.c
    src/drivers/block/blkpce.c
    src/drivers/block/blkqed.c
    src/drivers/block/blkram.c
    src/drivers/block/blkraw.c
    src/drivers/block/blkpart.c
    src/drivers/block/blkdosem.c
    src/drivers/pfdc/pfdc.c
    src/drivers/pfdc/pfdc-img.c
    src/drivers/pbit/pbit.c
    src/drivers/pbit/pbit-io.c
    src/drivers/pbit/pbit-io-pbit.c
    src/drivers/pbit/pbit-io-tc.c
    src/drivers/pbit/gcr-mac.c
    src/drivers/pbit/mfm-ibm.c
    src/drivers/sound/sound.c
    src/drivers/sound/filter.c
    src/drivers/sound/snd_null.c
    src/drivers/char/char.c

    # libini
    src/libini/read.c
    src/libini/write.c
    src/libini/scanner.c
    src/libini/section.c
    src/libini/strings.c
    src/libini/value.c
    src/libini/expr.c
)

#=============================================================================
# Main Executable
#=============================================================================
add_executable(${BUILD_NAME}
    # RP2350 platform integration
    src/rp2350_stubs.c
    src/fatfs_retarget.c
    src/rp2350_ini.c
    src/rp2350_mac.c

    src/tlsf.c
    src/main.c
    src/ini.c
    src/diskui.c
    src/settingsui.c
    src/config_save.c
    ${EMU_CORE_SOURCES}
)

target_include_directories(${BUILD_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/src/arch/macplus
    ${CMAKE_CURRENT_LIST_DIR}/src/libini
    ${CMAKE_CURRENT_LIST_DIR}/src/lib
    ${CMAKE_CURRENT_LIST_DIR}/src/cpu/e68000
    ${CMAKE_CURRENT_LIST_DIR}/src/chipset
    ${CMAKE_CURRENT_LIST_DIR}/src/devices
    ${CMAKE_CURRENT_LIST_DIR}/src/drivers
    ${CMAKE_CURRENT_LIST_DIR}/drivers/fatfs
    ${CMAKE_CURRENT_LIST_DIR}/drivers/sdcard
    ${CMAKE_CURRENT_LIST_DIR}/drivers/ps2kbd
    ${CMAKE_CURRENT_LIST_DIR}/drivers/ps2mouse
    ${CMAKE_CURRENT_LIST_DIR}/drivers/psram
    ${CMAKE_CURRENT_LIST_DIR}/drivers/vga
    ${CMAKE_CURRENT_LIST_DIR}/drivers/hdmi
    ${CMAKE_CURRENT_LIST_DIR}/drivers/audio
)
if(USB_HID_ENABLED)
    target_include_directories(${BUILD_NAME} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/drivers/usbhid
    )
endif()

if(AUDIO_TYPE STREQUAL "I2S")
    target_compile_definitions(${BUILD_NAME} PRIVATE FEATURE_AUDIO_I2S=1)
elseif(AUDIO_TYPE STREQUAL "PWM")
    target_compile_definitions(${BUILD_NAME} PRIVATE FEATURE_AUDIO_PWM=1)
endif()
if(DVI_A)
    target_compile_definitions(${BUILD_NAME} PRIVATE
        DVI_A=1
        DVI_VERTICAL_REPEAT=1
        PICO_CORE1_STACK_SIZE=0x200
    )
endif()
if(EMM)
    target_compile_definitions(${BUILD_NAME} PRIVATE EMULATE_LTEMS=1)
endif()

target_compile_definitions(${BUILD_NAME} PRIVATE
    PCE_VERSION_MAJOR=${PCE_VERSION_MAJOR}
    PCE_VERSION_MINOR=${PCE_VERSION_MINOR}
    PCE_VERSION_ITERATION=${PCE_VERSION_ITERATION}
    PCE_VERSION_STR="${PCE_VERSION_STR}"

    BOARD_${BOARD}
    BOARD_VARIANT_STR="${BOARD}"
    CPU_CLOCK_MHZ=${CPU_SPEED}
    CPU_VOLTAGE=${CPU_VOLTAGE}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}

    PICO_ON_DEVICE=1
    RP2350_BUILD=1
    PICO_RP2350=1

    SOUND_FREQUENCY=44100

    # config.h stubs - всё уndef, нужное задано выше
    PCE_DIR_SEP='/'
)

if(DEBUG_ENABLED)
    target_compile_definitions(${BUILD_NAME} PRIVATE DEBUG_ENABLED=1)
endif()
if(PROFILE_ENABLED)
    target_compile_definitions(${BUILD_NAME} PRIVATE PROFILE_IT=1)
endif()

target_compile_options(${BUILD_NAME} PRIVATE
    -O3
    -ffunction-sections
    -fdata-sections
)

target_link_libraries(${BUILD_NAME}
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_dma
    hardware_pio
    hardware_clocks
    hardware_vreg
    hardware_flash
    hardware_watchdog
    hardware_irq
    hardware_interp
    fatfs
    sdcard
    ps2kbd
    ps2mouse
    psram
    video_driver
    audio_driver
    usbhid
)
if(DVI_A)
    target_link_libraries(${BUILD_NAME} libdvi)
endif()

if(USB_HID_ENABLED)
    message(STATUS "USB HID enabled: Using UART for stdio (USB is Host mode for keyboard)")
    pico_enable_stdio_usb(${BUILD_NAME} 0)
    pico_enable_stdio_uart(${BUILD_NAME} 0)
else()
    pico_enable_stdio_usb(${BUILD_NAME} 1)
    pico_enable_stdio_uart(${BUILD_NAME} 0)
endif()

#=============================================================================
# Print configuration summary
#=============================================================================
message(STATUS "")
message(STATUS "PCE Build Configuration:")
message(STATUS "  Board Variant:  ${BOARD}")
message(STATUS "  EMM:            ${EMM}")
message(STATUS "  CPU Speed:      ${CPU_SPEED} MHz")
message(STATUS "  CPU Voltage:    ${CPU_VOLTAGE}")
message(STATUS "  PSRAM Speed:    ${PSRAM_SPEED} MHz")
message(STATUS "  Audio Type:     ${AUDIO_TYPE}")
message(STATUS "  DVI Audio:      ${DVI_A}")
message(STATUS "  USB HID:        ${USB_HID_ENABLED}")
message(STATUS "  Debug Output:   ${DEBUG_ENABLED}")
message(STATUS "  Build Name:     ${BUILD_NAME}")
message(STATUS "")

target_link_options(${BUILD_NAME} PRIVATE
    -Wl,--wrap=fopen
    -Wl,--wrap=fclose
    -Wl,--wrap=fread
    -Wl,--wrap=fwrite
    -Wl,--wrap=fseek
    -Wl,--wrap=ftell
    -Wl,--wrap=rewind
    -Wl,--wrap=feof
    -Wl,--wrap=ferror
    -Wl,--wrap=clearerr
    -Wl,--wrap=fflush
    -Wl,--wrap=fgetc
    -Wl,--wrap=ungetc
    -Wl,--wrap=fputc
    -Wl,--wrap=fputs
    -Wl,--wrap=fgets
    -Wl,--wrap=fprintf
    -Wl,--wrap=vfprintf
)

pico_add_extra_outputs(${BUILD_NAME})

target_link_options(${BUILD_NAME} PRIVATE -Xlinker --print-memory-usage --data-sections -Wl,--gc-sections)
