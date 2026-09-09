# Dance Dance Revolution 1st Mix — Recompiled

A static recompilation of **Dance Dance Revolution (Japan), SLPM-86222** built on
[PSXRecomp](https://github.com/mstan/psxrecomp). The MIPS code of the original
disc is translated to C and compiled into a native executable — not an emulator:
the game becomes a program your CPU runs directly.

**No game data is distributed here.** This repository holds only configuration,
build glue and tooling — about 35 KB. You supply your own legal disc dump, and
the recompiler produces the translated C on your machine.

---

## What you need

| | |
| --- | --- |
| **A legal disc dump** | Redump-style: one `.cue` plus 40 `.bin` tracks (track 1 is data, 2–40 are the CDDA music). |
| **A C toolchain** | CMake ≥ 3.20, Ninja and Clang. The PSXRecomp CLI can download a pinned `cmake-clang-v1` toolchain for you. |
| **Python 3** | To run the PSXRecomp CLI. |
| **~3 GB of disk** | The generated C alone is ~150 MB; the build tree is around 1.7 GB. |

The dump this was developed against, for reference:

```
Track 01 .bin   14,935,200 bytes   md5 57d6ae6568a77409abfc94ba1d695b62
SLPM_862.22      1,347,584 bytes   md5 374b74a901e5c0b8c8cb223318ffe234
```

A different revision may still work, but the recompiled addresses in
`game.toml` and the runtime fixes were derived from this one.

## Layout

Everything is in place after a recursive clone — PSXRecomp and recomp-ui are
submodules, pinned to the exact commits this build was made with:

```
ddr-1st-mix-recomp/
├── game.toml            identity, load address, entry PC, seeds
├── CMakeLists.txt
├── psxrecomp/           submodule — the framework, with the DDR fixes
├── recomp-ui/           submodule — launcher UI
├── game/                your dump goes here          (gitignored)
├── SLPM_862.22          boot executable from the disc (gitignored)
└── generated/           produced by the recompiler    (gitignored)
```

The framework is pinned deliberately. The fixes that make this game boot and
run — the CD DMA overread, the CDDA position compensation, the freeze
containment — live in the framework's runtime, not in this repository. Pointing
at upstream PSXRecomp instead would give you a build that hangs on the warning
screen before its first frame.

If you already keep one framework checkout shared across several games, a
sibling `../psxrecomp` directory is still honoured, and `-DPSXRECOMP_ROOT=<path>`
overrides both.

## Build

```bash
# 1. Clone with submodules — this brings the framework and the UI with it
git clone --recurse-submodules <this-repo> ddr-1st-mix-recomp
cd ddr-1st-mix-recomp

#    Already cloned without --recurse-submodules?
#      git submodule update --init --recursive

# 2. Put your dump in game/ — the .cue and all 40 .bin tracks.
#    game.toml expects this exact name:
#      game/Dance Dance Revolution (Japan).cue

# 3. Extract the boot executable SLPM_862.22 from the data track and put it
#    at the repository root. Any ISO9660 extractor will do (bchunk + 7z,
#    binmerge, a disc image mounter). game.toml reads it as `exe`.

# 4. Translate the game to C  (~150 MB into generated/, takes a while)
python psxrecomp/psxrecomp_cli.py generate \
  --config game.toml --project-root . \
  --disc "game/Dance Dance Revolution (Japan).cue"

# 5. Build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The executable lands in `build/`. `build/run.sh` wraps the whole loop — it
rebuilds first, aborts if the build fails (so you never test a stale binary),
and writes a timestamped log under `build/logs/`.

## Playing

Run the executable, or `./run.sh` from `build/`. On first launch the launcher
asks for the disc and writes a `settings.toml` beside the executable; that file
is machine-specific and is not tracked here.

`run.sh` exposes the runtime switches as environment variables — `./run.sh` with
no arguments plays the game clean. Run `head -30 build/run.sh` for the list.

---

## State of this build

Working: boot, correct animation speed, audio and arrows in sync, no OpenBIOS
splash, the 3D dancer, and an extended mode-select menu (the hidden **EDIT** and
**INFORMATION** entries are linked into the ring, plus a new **EXIT GAME** item
drawn with the game's own font).

### The freeze, and what is actually fixed

The game used to hard-freeze at random — mid-song or during the credits. The
cause was traced end to end:

1. `func_8006E7E0` returns an out-of-range animation index (`-1`, or an entry
   whose pointer was relocated against a stack base rather than the table base).
2. The switch code reads `table + index*8 + 12` and gets a pointer that is not
   an animation. The game's own guard only rejects a **zero** pointer, so the
   bad value passes.
3. The animation evaluator, handed that pointer (or a time of 0), returns 0 for
   body part 15.
4. Part 15 is drawn twice per frame — once at `base[15]`, once at
   `base[15] + offset[15]`. With offset 0 those are the same scene entry.
5. The same primitives are inserted twice into the same ordering-table bucket.
   Insertion is `prim->next = OT[b]; OT[b] = prim`, so a second pass over the
   same nodes closes the chain into a cycle.
6. The display-list merge walks that cycle forever. That is the freeze.

The same bad animation state is what made the dancer spin wildly and fly
off-screen; fixing the freeze fixed the dancer too.

**What ships here is containment, not a cure.** The runtime enforces the
invariant that was being violated — the same scene entry may not be drawn twice
between two ordering-table merges — by making the duplicate call return
immediately. This is safe: the suppressed draw would emit byte-identical
primitives, so nothing is lost visually. It fires roughly 20 times per session.

**Still open:** why `func_8006E7E0` returns `-1`, and why table entry `[0]` is
relocated to a stack address (`0x801F7FF8` instead of `0x800A4264`). Each of
those is an upstream defect that the containment merely absorbs.

## License

Build glue and tooling in this repository are provided as-is. It contains no
Konami code or assets. *Dance Dance Revolution* is a trademark of Konami; this
project is unaffiliated with and unendorsed by them.
