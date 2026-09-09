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
ddr-1st-recomp/
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

On Windows, run everything below from an **MSYS2** shell (Git Bash works too).
The build goes through a POSIX-shell path and `build/run.sh` is a bash script,
so cmd.exe and PowerShell are not enough. Linux and macOS need nothing special.
Where this shows `python`, MSYS2 usually wants `py`.

```bash
# 1. Clone with submodules — this brings the framework and the UI with it
git clone --recurse-submodules https://github.com/bmbelo87/ddr-1st-recomp.git
cd ddr-1st-recomp

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

# 5. Build. Go through the CLI: clang and ninja live in a pinned toolchain
#    cache, not on PATH, and the CLI resolves (or downloads) them for you.
python psxrecomp/psxrecomp_cli.py rebuild \
  --config game.toml --project-root . --build-dir build \
  --target ddr-1st-recomp --cmake-extra=-DPSX_DEBUG_TOOLS=ON
```

The `--cmake-extra=` needs the equals sign: argparse reads a value starting
with `-` as another option and refuses the command.

A plain `cmake -S . -B build -G Ninja` works too, but only if clang and ninja
are already on your PATH.

### Diagnostics are opt-in

The default build is a release build and is what you want. Everything the game
needs — the extended menu, the unlock mask, the containment that stops the
freeze — rides `psx_mod_function_entry`, the plugin hook that survives a
release build, from `src/ddr_hooks.c`. The addresses it attaches to are listed
in `game.toml` under `mod_function_entry_funcs`; changing that list is the one
thing here that needs a fresh `generate`.

Add `--cmake-extra=-DPSX_DEBUG_TOOLS=ON` to build the investigation tooling as
well: the ordering-table cycle detector, the write tracer, the primitive-buffer
watch, the GTE probe, and the F5 dancer toggle. They cost performance and are
not needed to play.

The executable lands in `build/`. `build/run.sh` wraps the whole loop — it
rebuilds first, aborts if the build fails (so you never test a stale binary),
and writes a timestamped log under `build/logs/`.

## Playing

From an MSYS2 shell:

```bash
cd build
./run.sh              # builds first, then launches
./run.sh mylabel      # same, and names the log build/logs/mylabel-<date>.log
```

`run.sh` rebuilds before every launch and **aborts if the build fails**, so you
can never end up testing a stale binary — a mistake that cost a whole debugging
session here once. It finds cmake through `CMakeCache.txt` when it is not on
PATH, which is what makes a plain MSYS2 shell work.

You can also double-click `Dance_Dance_Revolution_1st_Mix_Recompiled.exe` in
`build/`. That skips the rebuild and the log, but plays the same.

On first launch the launcher asks for the disc and writes a `settings.toml`
beside the executable; that file holds absolute paths for this machine and is
not tracked here.

`./run.sh` with no arguments plays the game clean. Every diagnostic is opt-in
through an environment variable — `UNLOCK=1` for the hidden modes and songs,
`CHAIN=1` for the ordering-table detector, and so on. The list is the comment
block at the top of the script:

```bash
head -30 build/run.sh
```

---

## Releases

A release here is **not** a playable download, and cannot be: the built
executable contains ~150 MB of C translated instruction by instruction from
Konami's boot executable. Shipping it would be shipping the game.

What ships instead is a **setup-host** zip, the model the PSXRecomp ecosystem
uses: the player downloads it, runs it, points it at their own legal disc, and
it generates and compiles on their machine. First run takes minutes; every run
after that starts immediately. Nothing in the download is Konami's.

The framework provides the machinery:

| | |
| --- | --- |
| `psxrecomp/tools/package_setup_host.sh` | builds the zip |
| `psxrecomp/docs/ci/templates/setup-release.yml` | GitHub Actions workflow, publishes on tag |
| `psxrecomp/docs/ci/HOST_ONLY_RELEASES.md` | what CI does, step by step |

Ready here:

- `[prepare_disc]` digests and `disc_crc` in `game.toml`, so a wrong dump is
  refused instead of silently producing garbage
- `catalog_identity.json` — identity, marketing metadata, TOC fingerprint
- `VERSION` + the CMake wiring that stamps it into the binary (the packager
  refuses to ship if the stamp and the release version disagree)

All of it is ready. The hooks were ported to `psx_mod_function_entry`, so a
release build carries them: a setup-host zip now produces the same game this
was tested on, not the pre-fix one. Building the zip is
`psxrecomp/tools/package_setup_host.sh`; publishing it on tag is the workflow
template above. Neither has been exercised from this repository yet.

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
