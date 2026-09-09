cmake_minimum_required(VERSION 3.20)
project(DDRecomplution)

if(NOT DEFINED PSXRECOMP_ROOT)
    get_filename_component(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../psxrecomp" ABSOLUTE)
endif()

if(NOT EXISTS "${PSXRECOMP_ROOT}/runtime/runtime.cmake")
    message(FATAL_ERROR
        "PSXRecomp runtime is missing: ${PSXRECOMP_ROOT}/runtime/runtime.cmake\n"
        "Set -DPSXRECOMP_ROOT=<path> or place psxrecomp next to DDRecomplution.")
endif()

include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")

file(GLOB GAME_FULL CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/*_full_*.c")
file(GLOB GAME_DISPATCH "${CMAKE_CURRENT_SOURCE_DIR}/generated/*_dispatch.c")

psxrecomp_add_runtime_target(DDRecomplution
    GAME_GENERATED_FULL_C     ${GAME_FULL}
    GAME_GENERATED_DISPATCH_C "${GAME_DISPATCH}"
    WINDOW_TITLE              "Dance Dance Revolution 1st Mix Recompiled"
    DEFAULT_GAME_CONFIG_PATH  "game.toml"
)
