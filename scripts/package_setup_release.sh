#!/usr/bin/env bash
#
# Build the setup-host zip for this title.
#
# A release here is not a playable download and cannot be: the built executable
# carries ~150 MB of C translated from Konami's boot executable. What ships is
# this zip -- the wizard plus this project's sources -- which the player runs
# against their own legal disc. It generates and compiles on their machine, so
# no game code ever leaves theirs.
#
# Usage (from the repository root, MSYS2 on Windows):
#   scripts/package_setup_release.sh <build-dir> <artifact> [recompiler-build]
#
#   scripts/package_setup_release.sh build-ci windows-x64
#
# Expects a setup-host build already configured and built in <build-dir>:
#
#   cmake -S . -B build-ci -G Ninja \
#     -DCMAKE_BUILD_TYPE=Release \
#     -DPSXRECOMP_FORCE_SETUP_HOST=ON \
#     -DPSXRECOMP_ALLOW_NO_BIOS=ON \
#     -DPSX_SETUP_WIZARD=ON
#   cmake --build build-ci
#
# PSX_SETUP_WIZARD is not optional with FORCE_SETUP_HOST: without it the zip
# opens to nothing, because there is no wizard to ask for the disc.
#
# Three things the packaging tools need on PATH / in the environment, none of
# which they can find on their own under MSYS2. Each one stops the run with a
# message that names the missing piece but not where to get it:
#
#   python   The tools probe for `python3` and `python`; MSYS2 users have `py`.
#              PYDIR=$(dirname "$(py -c 'import sys; print(sys.executable)' \
#                | tr -d '\r' | sed 's|\\|/|g; s|^\([A-Za-z]\):|/\L\1|')")
#              export PATH="$PYDIR:$PATH"
#
#   objdump  Used to work out which DLLs the emitters need. `pacman -S binutils`.
#
#   PSXRECOMP_RUNTIME_BIN_DIR
#            Where those DLLs live. The emitters link against the clang
#            toolchain's libc++.dll / libunwind.dll, and the default search path
#            is a MinGW location that does not exist here:
#              export PSXRECOMP_RUNTIME_BIN_DIR=\
#                "$HOME/.local/share/retcomm/toolchains/cmake-clang-v1/latest/bin"
#            (that is the WINDOWS home; under MSYS2 spell it /c/users/<you>/...)
#
# The DLLs ship beside psxrecomp-game.exe in the zip. They are not the game's
# dependency -- they belong to the tool that builds the game on the player's
# machine.
set -euo pipefail

BUILD_DIR="${1:?usage: $0 <build-dir> <artifact> [recompiler-build]}"
ARTIFACT="${2:?usage: $0 <build-dir> <artifact> [recompiler-build]}"
RECOMPILER_BUILD="${3:-psxrecomp/recompiler/build}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"

# The packager refuses to ship when the zip's version and the version stamped
# into the binary disagree -- a mismatch makes netplay lobby browsers filter
# each other out. VERSION is the single source; CMakeLists feeds it to
# PSX_GAME_VERSION, which the runtime stamps.
export RELEASE_VERSION="${RELEASE_VERSION:-$(tr -d '[:space:]' < VERSION)}"
echo "packaging ddr-1st-recomp ${RELEASE_VERSION} (${ARTIFACT})"

exec psxrecomp/tools/package_setup_host.sh \
  --build-dir        "${BUILD_DIR}" \
  --artifact         "${ARTIFACT}" \
  --recompiler-build "${RECOMPILER_BUILD}" \
  --zip-prefix       ddr1st \
  --exe-name         Dance_Dance_Revolution_1st_Mix_Recompiled \
  --display-name     "Dance Dance Revolution 1st Mix Recompiled" \
  --disc-hint        "your legally owned Dance Dance Revolution (Japan) disc, SLPM-86222" \
  --project-file     game.toml \
  --project-file     catalog_identity.json \
  --project-file     CMakeLists.txt \
  --project-file     VERSION \
  --project-file     README.md \
  --project-dir      src \
  --project-dir      mods \
  --project-dir      recompiler
