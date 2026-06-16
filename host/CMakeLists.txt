#
# ThumbyCue — host (Linux/SDL2) build.
#   cmake -B build_host -S host && cmake --build build_host -j8
#   ./build_host/thumbycue_host [pool|snooker]
#
cmake_minimum_required(VERSION 3.16)
project(thumbycue_host C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(GAME ${ROOT}/game)

find_package(SDL2 REQUIRED)

set(SRC
    ${CMAKE_CURRENT_LIST_DIR}/host_main.c
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

add_executable(thumbycue_host ${SRC})
target_compile_options(thumbycue_host PRIVATE
    -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -ffast-math)
target_include_directories(thumbycue_host PRIVATE
    ${GAME} ${ROOT}/device ${SDL2_INCLUDE_DIRS})
target_link_libraries(thumbycue_host PRIVATE ${SDL2_LIBRARIES} m)
