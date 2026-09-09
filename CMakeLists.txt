cmake_minimum_required(VERSION 3.20)
project(ddr-1st-recomp)

# PSXRecomp lives in the psxrecomp/ submodule. A sibling checkout still works
# as a fallback, for anyone who keeps one framework tree across several games.
if(NOT DEFINED PSXRECOMP_ROOT)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp/runtime/runtime.cmake")
        get_filename_component(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp" ABSOLUTE)
    else()
        get_filename_component(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../psxrecomp" ABSOLUTE)
    endif()
endif()

if(NOT EXISTS "${PSXRECOMP_ROOT}/runtime/runtime.cmake")
    message(FATAL_ERROR
        "PSXRecomp runtime is missing: ${PSXRECOMP_ROOT}/runtime/runtime.cmake\n"
        "The submodule is probably not checked out. Run:\n"
        "    git submodule update --init --recursive\n"
        "Or set -DPSXRECOMP_ROOT=<path> to point at your own checkout.")
endif()

# Release version. The runtime stamps this into the binary and writes
# psx_game_version.txt beside it; package_setup_host.sh refuses to ship a zip
# whose version disagrees with that stamp, so VERSION is the single source.
# Without this the stamp is the literal string "dev" and packaging fails.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PSX_GAME_VERSION LIMIT_COUNT 1)
string(STRIP "${PSX_GAME_VERSION}" PSX_GAME_VERSION)
message(STATUS "ddr-1st-recomp: version ${PSX_GAME_VERSION}")

include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")

file(GLOB GAME_FULL CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/*_full_*.c")
file(GLOB GAME_DISPATCH "${CMAKE_CURRENT_SOURCE_DIR}/generated/*_dispatch.c")

# Game hooks. These ride psx_mod_function_entry, which survives a release
# build -- unlike debug_server's entry hook, where they started life.
set(DDR_HOOKS "${CMAKE_CURRENT_SOURCE_DIR}/src/ddr_hooks.c")

psxrecomp_add_runtime_target(ddr-1st-recomp
    EXTRAS_SOURCES            ${DDR_HOOKS}
    GAME_GENERATED_FULL_C     ${GAME_FULL}
    GAME_GENERATED_DISPATCH_C "${GAME_DISPATCH}"
    WINDOW_TITLE              "Dance Dance Revolution 1st Mix Recompiled"
    DEFAULT_GAME_CONFIG_PATH  "game.toml"
)
