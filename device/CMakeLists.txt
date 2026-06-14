#
# ThumbyCue — device firmware (RP2350, Thumby Color), standalone.
#
#   cmake -S device -B build_device -DPICO_SDK_PATH=$HOME/mp-thumby/lib/pico-sdk
#   cmake --build build_device -j8
#   cp build_device/thumbycue.uf2 ../firmware_thumbycue.uf2
#
# Flash: turn OFF, hold DOWN d-pad, turn ON → mounts as RPI-RP2350 drive,
# copy the .uf2 onto it.
#
cmake_minimum_required(VERSION 3.16)

set(PICO_BOARD tinycircuits_thumby_color_rp2350)
set(PICO_PLATFORM rp2350)
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)

project(thumbycue_device C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)
pico_sdk_init()

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
    ${GAME}/cue_audio.c
    ${GAME}/r3d_raster.c
    ${GAME}/craft_font.c
)

target_include_directories(thumbycue PRIVATE ${GAME} ${DEVICE})

target_compile_options(thumbycue PRIVATE
    -O3 -g -Wall -Wextra
    -Wno-unused-parameter -Wno-unused-function -Wno-unused-but-set-variable
    -ffast-math
    -mcpu=cortex-m33 -mthumb -mfloat-abi=softfp -mfpu=fpv5-sp-d16)

target_link_libraries(thumbycue
    pico_stdlib pico_multicore pico_rand
    hardware_spi hardware_dma hardware_pwm hardware_irq
    hardware_sync hardware_clocks hardware_gpio)

target_compile_definitions(thumbycue PRIVATE
    PICO_HEAP_SIZE=0x4000
    PICO_STACK_SIZE=0x1000
    PICO_CORE1_STACK_SIZE=0x1000)

pico_enable_stdio_usb(thumbycue 0)
pico_enable_stdio_uart(thumbycue 0)
pico_add_extra_outputs(thumbycue)
