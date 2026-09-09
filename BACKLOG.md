# Backlog

Things known, measured, and deliberately not fixed yet. Kept here so the
knowledge does not have to be rediscovered.

## The freeze: root cause still open

The freeze is **contained**, not cured. `src/ddr_hooks.c` enforces the
invariant that was being violated, and a full session — songs plus the credits
— runs clean. The containment fires roughly 20 times per session, and each of
those is a symptom of something still wrong upstream.

The chain, traced end to end and confirmed by measurement at every step:

1. `func_8006E7E0` returns an out-of-range animation index. Two ways seen:
   `-1`, and a valid index whose table entry holds a pointer relocated against
   a **stack** base instead of the table's — `0x801F7FF8` where
   `0x800A3EB0 + 0x3B4 = 0x800A4264` was meant.
2. The switch at `0x80072B24` reads `table + index*8 + 12` and gets something
   that is not an animation. With index `-1` that lands on the table header,
   which holds `8` — and `8` is in the disc image, so the console reads the
   same value. The game's guard at `0x80072B3C` only rejects a **zero**
   pointer, so the bad value passes.
3. `func_8006D798`, handed that pointer (or a time of 0), returns 0 for body
   part 15.
4. Part 15 is drawn twice a frame — `func_80021AF0` at `base[15]` and again at
   `base[15] + offset[15]`. With offset 0 those are the same scene entry.
5. The same primitives are inserted twice into the same ordering-table bucket.
   Insertion is `prim->next = OT[b]; OT[b] = prim`, so a second pass over the
   same nodes closes the chain into a cycle.
6. `func_8007259C` walks that cycle forever.

The same bad animation state is what made the dancer spin wildly and fly
off-screen. Fixing the freeze fixed the dancer, which is the strongest evidence
the two were one defect.

**Where to resume:** why `func_8006E7E0` returns `-1`, and who writes the
mis-relocated table entry. The likely shape of the real fix is what the guard
already approximates — refuse the switch and keep a valid animation — but done
at the source, on the index, rather than downstream on the pointer.

**Tools, all still in the tree.** Build with
`--cmake-extra=-DPSX_DEBUG_TOOLS=ON` and see the header of `build/run.sh`:
`CHAIN=1` (block-chain check + duplicate detector), `TRACE=1` (ordering-table
cycle detector with a write trace), `PRIM=1` (primitive-buffer watch),
`OTP=1` (OT bucket probe), `GTE=<px>` (projection probe). `tools/dis.py`
disassembles any address in the boot executable.

## Two hypotheses that were tested and are dead

Recorded so nobody spends a day on them again:

- **Primitive-buffer overflow.** The buffers are 10240 bytes each, adjacent at
  `0x80010CD0` / `0x800134D0`. Measured peak during play: 5748 bytes, 56%.
  Never close.
- **Ordering-table bucket overflow.** The emitter masks the bucket to 8 bits
  (`(OTZ >> shift) & 0xFF`) and the model's OT at `0x800845F0` has exactly 256
  buckets, so the mask is the table size, not an oversight. The ~2.6M "out of
  range" events counted by an early probe were normal operation.

## Smaller items

- **949 seeds vs 1.** `probe_disc.py` reports 949 first-pass JAL seeds against
  the single entry point in `recompiler/seeds/seeds.txt`. Adopting them changes
  what the recompiler emits, so it needs its own test pass. May improve
  function discovery; may change nothing.
- **`strict = false`.** Set during bring-up. Worth revisiting once the seed
  list is wider.
