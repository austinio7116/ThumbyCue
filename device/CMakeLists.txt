#
# ThumbyCue — device firmware (RP2350, Thumby Color).
#
# Standalone:
#   cmake -S device -B build_device -DPICO_SDK_PATH=$HOME/mp-thumby/lib/pico-sdk
#   cmake --build build_device -j8
#   cp build_device/thumbycue.uf2 ../firmware_thumbycue.uf2
#
# As a ThumbyOne slot the parent CMakeLists sets THUMBYONE_SLOT_MODE and the
# THUMBYONE_*_SRC/INCLUDE vars, has already pico_sdk_init()'d, and rebases our
# UF2 into the slot region.
#
cmake_minimum_required(VERSION 3.16)

if (NOT THUMBYONE_SLOT_MODE)
    set(PICO_BOARD tinycircuits_thumby_color_rp2350)
    set(PICO_PLATFORM rp2350)
    include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
    project(thumbycue_device C CXX ASM)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 14)
    pico_sdk_init()
endif()

set(ROOT   ${CMAKE_CURRENT_LIST_DIR}/..)
set(GAME   ${ROOT}/game)
set(DEVICE ${CMAKE_CURRENT_LIST_DIR})

add_executable(thumbycue
    ${DEVICE}/cue_device_main.c
    ${DEVICE}/craft_lcd_gc9107.c
    ${DEVICE}/craft_buttons.c
    ${DEVICE}/craft_rumble.c
    ${DEVICE}/craft_audio_pwm.c

    ${GAME}/cue_game.c
    ${GAME}/cue_render.c
    ${GAME}/cue_physics.c
    ${GAME}/cue_table.c
    ${GAME}/cue_rules.c
    ${GAME}/cue_ai.c
    ${GAME}/cue_audio.c
    ${GAME}/r3d_raster.c
    ${GAME}/craft_font.c
)

# Slot mode: ThumbyOne shared helpers (FatFs, FS mount, LED, backlight,
# settings, handoff for return-to-lobby). No game save backend — ThumbyCue
# keeps no persistent state; only the lobby's volume/brightness are honoured.
if (THUMBYONE_SLOT_MODE)
    target_sources(thumbycue PRIVATE
        ${THUMBYONE_COMMON_SRC}
        ${THUMBYONE_FATFS_SRC}
        ${THUMBYONE_COMMON_FS_SRC}
        ${THUMBYONE_COMMON_LED_SRC}
        ${THUMBYONE_COMMON_BACKLIGHT_SRC}
        ${THUMBYONE_COMMON_SETTINGS_SRC}
    )
endif()

target_include_directories(thumbycue PRIVATE ${GAME} ${DEVICE})
if (THUMBYONE_SLOT_MODE)
    target_include_directories(thumbycue PRIVATE
        ${THUMBYONE_FATFS_INCLUDE}
        ${THUMBYONE_COMMON_INCLUDE}
        ${THUMBYONE_COMMON_LED_INCLUDE}
        ${THUMBYONE_COMMON_BACKLIGHT_INCLUDE}
        ${THUMBYONE_COMMON_SETTINGS_INCLUDE}
    )
endif()

target_compile_options(thumbycue PRIVATE
    -O3 -g -Wall -Wextra
    -Wno-unused-parameter -Wno-unused-function -Wno-unused-but-set-variable
    -ffast-math
    -mcpu=cortex-m33 -mthumb -mfloat-abi=softfp -mfpu=fpv5-sp-d16)

target_link_libraries(thumbycue
    pico_stdlib pico_multicore pico_rand
    hardware_spi hardware_dma hardware_pwm hardware_irq
    hardware_flash hardware_sync hardware_clocks hardware_gpio)
if (THUMBYONE_SLOT_MODE)
    target_link_libraries(thumbycue hardware_pio hardware_adc)
endif()

target_compile_definitions(thumbycue PRIVATE
    PICO_HEAP_SIZE=0x4000
    PICO_STACK_SIZE=0x1000
    PICO_CORE1_STACK_SIZE=0x1000
    $<$<BOOL:${THUMBYONE_SLOT_MODE}>:THUMBYONE_SLOT_MODE=1>
    $<$<BOOL:${THUMBYONE_SLOT_MODE}>:THUMBYONE_DISK_STATIC_MERGE_BUF=1>)

pico_enable_stdio_usb(thumbycue 0)
pico_enable_stdio_uart(thumbycue 0)

if (THUMBYONE_SLOT_MODE)
    # absolute UF2 family so the lobby's chain_image loads us at our rebase.
    pico_set_uf2_family(thumbycue "absolute")
endif()

pico_add_extra_outputs(thumbycue)

if (THUMBYONE_SLOT_MODE)
    set_target_properties(thumbycue PROPERTIES OUTPUT_NAME "firmware_thumbycue")
    # Fail the build if the image exceeds the 512 KB ROGUE-sized slot.
    add_custom_command(TARGET thumbycue POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DBIN=$<TARGET_FILE_DIR:thumbycue>/firmware_thumbycue.bin
            -DLIMIT=524288
            -P ${CMAKE_CURRENT_LIST_DIR}/check_size.cmake)
endif()
