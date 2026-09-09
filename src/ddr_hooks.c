/*
 * Dance Dance Revolution 1st Mix (SLPM-86222) — game hooks.
 *
 * Everything here rides psx_mod_function_entry, the trusted plugin hook that
 * survives a release build. The recompiler emits a call to it at each address
 * listed in game.toml's [recompiler] mod_function_entry_funcs, so changing
 * that list is the one thing here that needs a fresh recompiler pass.
 *
 * This deliberately lives in the GAME repository, not in the framework: the
 * framework fork carries only the two general fixes (the CD DMA sector-FIFO
 * overread and the CDDA audible-position compensation), so it stays
 * upstreamable without dragging DDR specifics along.
 *
 * These were developed against debug_server.c's entry hook, which the
 * framework compiles out of a release build (PSX_NO_DEBUG_TOOLS). That made
 * the whole feature set — including the fix for a hard freeze — silently
 * vanish from exactly the build a player would download. The diagnostics that
 * found these problems stay behind that flag, where they belong; what a player
 * needs is here.
 */
#include "mod_plugins.h"
#include "cpu_state.h"

#include <stdio.h>
#include <string.h>

/* ── Addresses, all verified by disassembly during bring-up ──────────────── */

#define DDR_MENU_SCREEN   0x8004A380u  /* mode-select tick: reads pad, moves cursor */
#define DDR_MENU_DRAW     0x80049F9Cu  /* draws the item list; a1 = highlighted item */
#define DDR_MODEL_EMIT    0x80022268u  /* walks one scene entry's block chain */
#define DDR_OT_MERGE      0x8007259Cu  /* consumes the ordering table, once a frame */
#define DDR_ANIM_DRIVER   0x8006F3E8u  /* reads the animation pointer + time */

#define MENU_TABLE    0x8007ECE8u   /* item table, 12 bytes per entry */
#define MENU_STRIDE   12u
#define MENU_CURSOR   0x8009706Cu   /* selected item (a word) */
#define MENU_PADPTR   0x80073580u   /* -> input struct; new-press mask at +84 */
#define MENU_PLAYERS  2u
#define MENU_PAD_PITCH 16u
#define MENU_PAD_OFF  84u
#define MENU_CONFIRM  0x0820u       /* R1 | Right */
#define MENU_UP_MASK  0x1000u       /* Triangle — "up" on the dance mat */
#define MENU_DOWN_MASK 0x4000u      /* Cross    — "down" on the dance mat */
#define MENU_NAV_MASK (MENU_UP_MASK | MENU_DOWN_MASK)

#define MENU_ITEMS    7u            /* 0-4 stock, 5 = EDIT, 6 = INFORMATION */
#define MENU_Y0       60u
#define MENU_YSTEP    15u

#define MENU_TEXT_FUNC 0x80020D30u  /* func(x, y, string, font) */
#define MENU_TEXT_RET  0x8004A5B0u  /* the return address the game itself uses */
#define MENU_STR_ADDR  0x80077500u  /* a 4203-byte run of zeros; no code there */
#define MENU_EXIT_X    (-36)        /* "EXIT GAME": 9 glyphs of 8px, centred */
#define MENU_TEXT_OFFY 150          /* sprite Y minus text Y, measured on screen */
#define MENU_EXIT_FONT     0x100u   /* bits 8-9 pick the font, not a scale */
#define MENU_EXIT_FONT_SEL 0x000u

#define UNLOCK_MASK_ADDR 0x800103F0u
#define UNLOCK_SONG_ADDR 0x8001046Du
#define UNLOCK_MASK_ALL  0x1Fu

#define ANIM_PTR_OFF  1472u
#define ANIM_TIME_OFF 1464u
#define ANIM_PTR_TOP  0x801E0000u   /* above this is stack, not animation data */

#define CHAIN_MAX 256u

/* ── Small helpers over the narrow guest services ────────────────────────── */

static int in_ram(uint32_t a) { return a >= 0x80000000u && a < 0x80200000u; }

static uint32_t pad_read(uint32_t base)
{
    uint32_t np = 0;
    for (uint32_t p = 0; p < MENU_PLAYERS; p++)
        np |= psx_mod_read_word(base + p * MENU_PAD_PITCH + MENU_PAD_OFF);
    return np;
}

/* Clearing bits here works because this runs at the ENTRY of the screen tick,
 * before the game reads the buffer. The game then simply sees no input. */
static void pad_eat(uint32_t base, uint32_t mask)
{
    for (uint32_t p = 0; p < MENU_PLAYERS; p++) {
        uint32_t a = base + p * MENU_PAD_PITCH + MENU_PAD_OFF;
        psx_mod_write_word(a, psx_mod_read_word(a) & ~mask);
    }
}

/* ── The menu: EDIT, INFORMATION and a new EXIT GAME ─────────────────────────
 *
 * The mode-select list is a table of 12-byte entries at 0x8007ECE8 where byte 0
 * is the "up" neighbour and byte 1 the "down" one — a ring. Entries 5 and 6
 * already exist complete, with their own sprite, id and record; Konami just
 * left them out of the ring. Linking them costs four bytes each and gives EDIT
 * and INFORMATION back, drawn by the game itself.
 *
 * EXIT GAME cannot be an eighth entry: the table ends at 0x8007ED3C where a
 * function-pointer table begins, and no index from 7 to 255 lands anywhere
 * free. So it is not an entry — it is drawn as text and navigated here.
 */

static int      s_menu_linked;
static int      s_exit_sel;         /* the virtual item is selected */
static int      s_exit_str_done;
static int      s_exit_in_call;     /* re-entry guard for the nested call */

static void menu_link_ring(void)
{
    for (uint32_t i = 0; i < MENU_ITEMS; i++) {
        uint32_t e = MENU_TABLE + i * MENU_STRIDE;
        psx_mod_write_byte(e + 0u, (uint8_t)((i + MENU_ITEMS - 1u) % MENU_ITEMS));
        psx_mod_write_byte(e + 1u, (uint8_t)((i + 1u) % MENU_ITEMS));

        /* The two hidden entries both shipped at y=152, one on top of the
         * other, because they were never meant to be on screen together.
         * Respace the whole list from one origin. */
        uint32_t rec = psx_mod_read_word(e + 8u);
        if (in_ram(rec)) {
            uint32_t y = MENU_Y0 + i * MENU_YSTEP;
            psx_mod_write_byte(rec + 2u, (uint8_t)(y & 0xFFu));
            psx_mod_write_byte(rec + 3u, (uint8_t)(y >> 8));
        }
    }
}

static uint32_t exit_string(void)
{
    if (!s_exit_str_done) {
        static const char txt[] = "EXIT GAME";
        uint32_t i = 0;
        for (; txt[i]; i++) psx_mod_write_byte(MENU_STR_ADDR + i, (uint8_t)txt[i]);
        psx_mod_write_byte(MENU_STR_ADDR + i, 0u);
        s_exit_str_done = 1;
    }
    return MENU_STR_ADDR;
}

/*
 * Draw one line with the game's own text routine, as a nested guest call —
 * the same contract the BIOS HLE uses for event callbacks. Registers are saved
 * and restored because the caller has not spilled its own yet.
 */
static void draw_text(CPUState *cpu, int x, int y, uint32_t str, uint32_t font)
{
    extern void psx_dispatch_call(CPUState *cpu, uint32_t addr, uint32_t ret);
    if (!cpu || s_exit_in_call) return;

    uint32_t gpr[32];
    memcpy(gpr, cpu->gpr, sizeof gpr);
    uint32_t pc = cpu->pc, hi = cpu->hi, lo = cpu->lo;

    cpu->gpr[4]  = (uint32_t)(int32_t)x;
    cpu->gpr[5]  = (uint32_t)(int32_t)y;
    cpu->gpr[6]  = str;
    cpu->gpr[7]  = font;
    cpu->gpr[31] = MENU_TEXT_RET;
    cpu->pc      = 0;

    s_exit_in_call = 1;
    psx_dispatch_call(cpu, MENU_TEXT_FUNC, MENU_TEXT_RET);
    s_exit_in_call = 0;

    memcpy(cpu->gpr, gpr, sizeof gpr);
    cpu->pc = pc; cpu->hi = hi; cpu->lo = lo;
}

/* Entry of the mode-select tick: navigation, and the confirm that quits. */
static void on_menu_screen(CPUState *cpu, uint32_t addr)
{
    (void)addr;
    if (!s_menu_linked) {
        menu_link_ring();
        s_menu_linked = 1;
    }

    uint32_t st = psx_mod_read_word(MENU_PADPTR);
    if (!st) return;

    uint32_t np      = pad_read(st);
    uint32_t cursor  = psx_mod_read_word(MENU_CURSOR);
    int      up      = (np & MENU_UP_MASK)   != 0;
    int      down    = (np & MENU_DOWN_MASK) != 0;
    int      confirm = (np & MENU_CONFIRM)   != 0;

    if (!s_exit_sel) {
        if (cursor == MENU_ITEMS - 1u && down) {   /* down from INFORMATION */
            s_exit_sel = 1;
            pad_eat(st, MENU_NAV_MASK | MENU_CONFIRM);
        }
    } else if (up) {
        s_exit_sel = 0;                            /* back to INFORMATION */
        pad_eat(st, MENU_NAV_MASK | MENU_CONFIRM);
    } else if (down) {
        s_exit_sel = 0;                            /* wrap to the top */
        psx_mod_write_word(MENU_CURSOR, 0u);
        pad_eat(st, MENU_NAV_MASK | MENU_CONFIRM);
    } else if (confirm) {
        extern void psx_request_quit(void);
        pad_eat(st, MENU_CONFIRM);
        psx_request_quit();
    }
}

/*
 * Entry of the list drawer, func(mode, highlighted_item, brightness).
 *
 * Drawing here rather than beside the game's own header call is what keeps the
 * line steady: the header is gated on two bits of a frame counter
 * ([ctx+32] & 0x30), so anything drawn with it inherits that blink. This
 * drawer runs every frame, in the same drawing phase.
 *
 * Only the item in a1 gets the highlight, so with EXIT GAME selected a1 is set
 * to an index that does not exist and no real item lights up. That write comes
 * AFTER the text, because draw_text saves and restores every register.
 */
static void on_menu_draw(CPUState *cpu, uint32_t addr)
{
    (void)addr;
    if (!cpu || s_exit_in_call || !s_menu_linked) return;

    int      y    = (int)(MENU_Y0 + MENU_ITEMS * MENU_YSTEP) - MENU_TEXT_OFFY;
    uint32_t font = s_exit_sel ? MENU_EXIT_FONT_SEL : MENU_EXIT_FONT;
    draw_text(cpu, MENU_EXIT_X, y, exit_string(), font);

    if (s_exit_sel) cpu->gpr[5] = 0xFFu;
}

/* ── The freeze ──────────────────────────────────────────────────────────────
 *
 * Traced end to end during bring-up:
 *
 *   func_8006E7E0 returns an out-of-range animation index (-1, or an entry
 *   whose pointer was relocated against a stack base instead of the table's)
 *     -> the switch reads table + index*8 + 12 and gets something that is not
 *        an animation; the game's own guard only rejects a ZERO pointer, so it
 *        passes
 *     -> the evaluator, handed that pointer or a time of 0, returns 0 for body
 *        part 15
 *     -> part 15 is drawn twice a frame, at base[15] and base[15]+offset[15];
 *        with offset 0 those are the same scene entry
 *     -> the same primitives are inserted twice into the same OT bucket, and
 *        since insertion is "prim->next = OT[b]; OT[b] = prim", the second
 *        pass closes the chain into a cycle
 *     -> the display-list merge walks that cycle forever.
 *
 * Two containments, at two different layers. Neither is a cure: the -1 and the
 * mis-relocated table entry are still open.
 */

/* Guard 1 — refuse an animation pointer that is not one.
 *
 * "Is it a RAM address" is too weak: the mis-relocated entry is 0x801F7FF8,
 * which passes that and still brings everything down. So the range excludes
 * the stack region, and the CONTENT is checked too: the evaluator reads a part
 * count at [ptr+4], and an absurd count means this is not animation data. */
static uint32_t s_anim_last[4];

static int anim_ptr_ok(uint32_t p)
{
    if (p < 0x80010000u || p >= ANIM_PTR_TOP || (p & 3u) != 0u) return 0;
    uint32_t n = psx_mod_read_word(p + 4u) & 0xFFFFu;
    return n > 0u && n <= 256u;
}

static void on_anim_driver(CPUState *cpu, uint32_t addr)
{
    (void)addr;
    if (!cpu) return;
    uint32_t blk = cpu->gpr[4];
    if (!in_ram(blk)) return;

    uint32_t cur  = psx_mod_read_word(blk + ANIM_PTR_OFF);
    uint32_t slot = (blk >> 3) & 3u;

    if (anim_ptr_ok(cur)) { s_anim_last[slot] = cur; return; }
    if (!anim_ptr_ok(s_anim_last[slot])) return;

    /* Keep the previous animation: exactly what the game's own "skip" branch
     * would have done had the garbage it read happened to be zero. */
    psx_mod_write_word(blk + ANIM_PTR_OFF, s_anim_last[slot]);
    psx_mod_write_word(blk + ANIM_TIME_OFF, 0u);
}

/* Guard 2 — one scene entry may not be drawn twice between two OT merges.
 *
 * This is the invariant that was actually being violated, and it catches every
 * route to the collapse, including the one where the pointer is valid and the
 * animation time is simply 0. The suppressed draw would emit byte-identical
 * primitives, so nothing is lost on screen.
 *
 * Suppression without touching game data: func_80022268 returns immediately
 * when the object count is zero, so a0 is pointed at a scratch struct whose
 * count is zero and the body never runs. */
static uint32_t s_seen[CHAIN_MAX];
static uint32_t s_seen_n;
static uint32_t s_scratch;

static uint32_t dedup_scratch(void)
{
    if (!s_scratch) {
        s_scratch = MENU_STR_ADDR + 0x200u;      /* same zero run, past the string */
        psx_mod_write_word(s_scratch + 8u,  s_scratch + 0x20u);
        psx_mod_write_word(s_scratch + 12u, s_scratch + 0x20u);
        psx_mod_write_word(s_scratch + 0x20u, 0u);
        psx_mod_write_word(s_scratch + 0x24u, 0u);   /* object count = 0 */
    }
    return s_scratch;
}

static void on_model_emit(CPUState *cpu, uint32_t addr)
{
    (void)addr;
    if (!cpu) return;

    uint32_t a0    = cpu->gpr[4];
    uint32_t hdr   = psx_mod_read_word(a0 + 8u);
    if (!in_ram(hdr)) return;
    uint32_t count = psx_mod_read_word(hdr + 4u);
    uint32_t node  = psx_mod_read_word(a0 + 12u);
    if (!count || count > CHAIN_MAX) return;

    for (uint32_t i = 0; i < count; i++) {
        if (!in_ram(node)) return;
        for (uint32_t k = 0; k < s_seen_n; k++) {
            if (s_seen[k] != node) continue;
            cpu->gpr[4] = dedup_scratch();      /* draw nothing */
            return;
        }
        if (s_seen_n >= CHAIN_MAX) return;
        s_seen[s_seen_n++] = node;
        node = 0x80000000u | (psx_mod_read_word(node) & 0x00FFFFFFu);
    }
}

/* ── Unlocks, and the interval boundary ─────────────────────────────────────
 *
 * The ladder is computed in memory, not read from disc: a save block at
 * 0x800103D0 holds a 3x5 table of play counters, and two routines sum it and
 * light bits in a mask at 0x800103F0 (>=10 -> 0x01, >=100 -> 0x08, >=500 ->
 * 0x10 = EDIT; the +36 sum drives 0x02/0x04 and the song byte at 0x8001046D).
 *
 * Only lighting bits, never clearing, is what the game's own routines do (they
 * use ori), so this coexists with real progress. It does NOT touch the play
 * counters, so nothing reaches the memory card.
 */
static int s_unlock_on = 1;

static void on_ot_merge(CPUState *cpu, uint32_t addr)
{
    (void)cpu; (void)addr;

    /* The list has just been consumed: a primitive repeated after this point
     * is a new frame's, not a duplicate. This is the real interval, and using
     * it instead of a host frame counter is what makes the check honest. */
    s_seen_n = 0;

    if (!s_unlock_on || !s_menu_linked) return;
    uint32_t cur = psx_mod_read_word(UNLOCK_MASK_ADDR);
    if ((cur & UNLOCK_MASK_ALL) != UNLOCK_MASK_ALL)
        psx_mod_write_word(UNLOCK_MASK_ADDR, cur | UNLOCK_MASK_ALL);
    uint8_t b = psx_mod_read_byte(UNLOCK_SONG_ADDR);
    if (!(b & 1u)) psx_mod_write_byte(UNLOCK_SONG_ADDR, (uint8_t)(b | 1u));
}

/* ── Registration ───────────────────────────────────────────────────────────
 * Addresses must also be listed in game.toml [recompiler]
 * mod_function_entry_funcs, or these callbacks are simply never reached. */
PSX_MOD_CONSTRUCTOR(ddr_register_hooks)
{
    (void)psx_mod_register_function_entry_plugin("ddr.menu",   DDR_MENU_SCREEN, on_menu_screen);
    (void)psx_mod_register_function_entry_plugin("ddr.menu",   DDR_MENU_DRAW,   on_menu_draw);
    (void)psx_mod_register_function_entry_plugin("ddr.dedup",  DDR_MODEL_EMIT,  on_model_emit);
    (void)psx_mod_register_function_entry_plugin("ddr.dedup",  DDR_OT_MERGE,    on_ot_merge);
    (void)psx_mod_register_function_entry_plugin("ddr.anim",   DDR_ANIM_DRIVER, on_anim_driver);
}
