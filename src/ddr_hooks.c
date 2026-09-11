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
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Addresses, all verified by disassembly during bring-up ──────────────── */

#define DDR_MENU_SCREEN   0x8004A380u  /* mode-select tick: reads pad, moves cursor */
#define DDR_MENU_DRAW     0x80049F9Cu  /* draws the item list; a1 = highlighted item */
#define DDR_MODEL_EMIT    0x80022268u  /* walks one scene entry's block chain */
#define DDR_OT_MERGE      0x8007259Cu  /* consumes the ordering table, once a frame */
#define DDR_ANIM_DRIVER   0x8006F3E8u  /* reads the animation pointer + time */

/* The frame loop (0x8001E414..0x8001E5FC, one GsDrawOt per turn) waits at
 * 0x8001E500: VSync(n), with n taken from this byte -- 1 is mapped to 0
 * ("wait for the next field", 60 Hz) and any other value is passed through,
 * so 2 means "wait two fields", exactly 30 Hz. */
#define VSYNC_MODE_ADDR   0x8008C4E8u

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

#define MENU_SFX_FUNC  0x8002508Cu  /* func(sound id) */
#define MENU_SFX_MOVE  0x7F000603u  /* cursor moved between items */
#define MENU_SFX_ENTER 0x7F00060Bu  /* item confirmed */

#define MENU_TEXT_FUNC 0x80020D30u  /* func(x, y, string, font) */
#define MENU_TEXT_RET  0x8004A5B0u  /* the return address the game itself uses */
#define MENU_STR_ADDR  0x80077500u  /* a 4203-byte run of zeros; no code there */
#define MENU_EXIT_X    (-36)        /* "EXIT GAME": 9 glyphs of 8px, centred */
#define MENU_TEXT_OFFY 150          /* sprite Y minus text Y, measured on screen */
#define MENU_EXIT_FONT     0x100u   /* bits 8-9 pick the font, not a scale */

/* Selection is a colour, not a different font.
 *
 * Every glyph primitive carries R, G, B at +4, +5, +6, and the game writes the
 * same value to all three (128 = neutral, the texture comes through as-is).
 * The modulation is per-channel and multiplicative, so driving the channels
 * apart shifts the font's yellow and its blue outline towards other colours --
 * not a true hue rotation, but the effect asked for, and above 128 a channel
 * brightens rather than only darkening.
 *
 * PSX_MENU_SEL_RGB="r,g,b" overrides at run time, which is the point: this is
 * worth trying values on rather than arguing about. */
#define MENU_SEL_R 255u
#define MENU_SEL_G  96u
#define MENU_SEL_B  96u

#define PRIM_PTR_OFF 156u          /* [ctx+156] -- the primitive bump pointer */
#define PRIM_STARTS  0x80084398u   /* the two buffer starts (double buffered) */

/* Song background: a lattice of textured quads, not one image.
 *
 * A captured frame settles it -- 35 PolyFT4 of exactly 64x32 at ordering-table
 * rank 0 (drawn first, so: background), tiling x = 0,64,128,192,256 by
 * y = 0..192, all carrying colour 122,122,122. The game already dims them a
 * little; 128 would be neutral.
 *
 * Attribution could not name the emitter -- the whole table is submitted by one
 * DMA, so every packet is credited to the BIOS -- and the real draw goes
 * through an indirect call (jalr at 0x8003EDF0) into the object's own method.
 * Chasing that means unpicking a method table. The geometry is a far better
 * handle than the call site: quad, 64 by 32, textured. Nothing else in the
 * frame looks like that.
 */
#define BG_TILE_W 64
#define BG_TILE_H 32
#define BG_PRIM_WORDS 9u           /* PolyFT4: 10 words, so the length byte is 9 */

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

/*
 * All items as text, instead of the disc's sprites.
 *
 * The five stock entries plus EDIT and INFORMATION are sprites: prebaked
 * images in VRAM, pointed at by the record at entry+8. That is why EXIT GAME
 * needed a different route -- there is no "EXIT GAME" image on the disc.
 *
 * Drawing every line with the same generic font makes menu items TEXT, and
 * text is free: adding, renaming or reordering costs a line in this table
 * instead of new artwork. The cost is the Konami lettering, which is nicer
 * than the system font. PSX_MENU_SPRITES=1 puts the original art back.
 *
 * Order is screen order, which is index order because menu_link_ring() spaces
 * item i at MENU_Y0 + i*MENU_YSTEP. Spellings are inferred, not read out of
 * the sprites -- correcting one is a one-line edit.
 */
static const char *const MENU_LABELS[] = {
    "ARCADE MODE GAME",
    "ARRANGE MODE GAME",
    "TRAINING",
    "RECORDS",
    "OPTION",
    "EDIT",
    "INFORMATION",
    "EXIT GAME",          /* the virtual item; no entry in the game's table */
};
#define MENU_LABEL_COUNT ((int)(sizeof MENU_LABELS / sizeof MENU_LABELS[0]))

/* One scratch string per label, written once into the zero run. 32 bytes each
 * is comfortably above the longest label. */
#define MENU_LABEL_SLOT 32u

/*
 * Feature flags, driven by the mods/preloaded/packages/ddr.menu catalog.
 *
 * They start OFF and an activation callback turns them on, which is the
 * framework's pattern (see mod_builtin_bezel.c): a disabled feature simply
 * never activates. The consequence is worth stating -- a build whose mod
 * catalog is missing runs the STOCK menu, because nothing switched these on.
 * That is why the packager must not be passed --no-mods any more.
 *
 * The freeze containment is deliberately NOT a feature. It is not a preference
 * and must never be off.
 */
static int      s_feat_extended;
static int      s_feat_text;
static int      s_feat_unlocks;
static int      s_feat_no_dancer;
static int      s_feat_dim_bg;

static void ddr_activate_extended(void) { s_feat_extended = 1; }
static void ddr_activate_text(void)     { s_feat_text     = 1; }
static void ddr_activate_unlocks(void)  { s_feat_unlocks  = 1; }
static void ddr_activate_no_dancer(void){ s_feat_no_dancer = 1; }
static void ddr_activate_dim_bg(void)   { s_feat_dim_bg    = 1; }
static int      s_feat_bga_dark;
static void ddr_activate_bga_dark(void) { s_feat_bga_dark  = 1; }
static int      s_feat_fps60;
static void ddr_activate_fps60(void)    { s_feat_fps60     = 1; }

/* One declared option, as an integer, with the manifest default as fallback. */
static int option_int_pkg(const char *pkg, const char *feature,
                          const char *opt, int fallback)
{
    char buf[32];
    if (psx_mod_option_value(pkg, feature, opt, buf, sizeof buf) && buf[0]) {
        int v = atoi(buf);
        if (v >= 0 && v <= 255) return v;
    }
    return fallback;
}

static int option_int(const char *feature, const char *opt, int fallback)
{
    return option_int_pkg("ddr.menu", feature, opt, fallback);
}

static int      s_labels_done;
static int      s_sprites_hidden;
static int      s_menu_linked;
static int      s_exit_sel;         /* the virtual item is selected */
static int      s_exit_str_done;
static int      s_exit_in_call;     /* re-entry guard for the nested call */

/*
 * Is the ring still ours?
 *
 * Entering the mode-select screen runs an init routine (around 0x80049E8C)
 * that rebuilds the navigation table, so linking once at startup is not
 * enough: leave the screen and come back and the extra items are unreachable
 * again.
 *
 * Checking a single link is not enough either, and that mistake is worth
 * recording. That routine takes parameters (the branches at 0x80049F24 and
 * 0x80049F2C) and builds a ring of five, six or seven items depending on where
 * it was called from. Coming back from a submenu it builds SIX -- so item 4
 * still points at item 5, a one-byte check says "ours", and the 5 -> 6 link
 * stays broken with EDIT reachable and INFORMATION not. Verify every link.
 */
static int ring_is_ours(void)
{
    for (uint32_t i = 0; i < MENU_ITEMS; i++) {
        uint32_t e = MENU_TABLE + i * MENU_STRIDE;
        if (psx_mod_read_byte(e + 0u) != (i + MENU_ITEMS - 1u) % MENU_ITEMS) return 0;
        if (psx_mod_read_byte(e + 1u) != (i + 1u) % MENU_ITEMS) return 0;
    }
    return 1;
}

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

static uint32_t label_string(int i);
static int      label_x(int i);
static void     draw_text(CPUState *cpu, int x, int y, uint32_t str, uint32_t font);

/*
 * Dim the background tiles.
 *
 * Walked at the ordering-table merge, when every primitive of the frame exists
 * and none has been consumed. The buffer is rebuilt each frame, so scaling is
 * applied once to fresh values and never compounds.
 */
static void dim_background(int percent)
{
    if (percent >= 100) return;
    uint32_t ctx = psx_mod_read_word(MENU_PADPTR);
    if (!ctx) return;
    uint32_t cur = psx_mod_read_word(ctx + PRIM_PTR_OFF);
    uint32_t s0  = psx_mod_read_word(PRIM_STARTS);
    uint32_t s1  = psx_mod_read_word(PRIM_STARTS + 4u);
    if (!in_ram(cur) || !in_ram(s0) || !in_ram(s1)) return;

    /* Which of the two buffers this frame used: the nearest start below. */
    uint32_t p = (cur >= s1 && s1 > s0) ? s1 : ((cur >= s0) ? s0 : 0u);
    if (!p) return;

    for (int guard = 0; p < cur && guard < 4096; guard++) {
        uint32_t len = psx_mod_read_byte(p + 3u);
        if (len == 0u) break;
        if (len == BG_PRIM_WORDS) {
            uint32_t cmd = psx_mod_read_byte(p + 7u);
            if (cmd >= 0x2Cu && cmd <= 0x2Fu) {          /* textured quad */
                int x0 = (int16_t)psx_mod_read_half(p + 8u);
                int y0 = (int16_t)psx_mod_read_half(p + 10u);
                int x3 = (int16_t)psx_mod_read_half(p + 32u);
                int y3 = (int16_t)psx_mod_read_half(p + 34u);
                int w  = x3 - x0, h = y3 - y0;
                if (w < 0) w = -w;
                if (h < 0) h = -h;
                if (w == BG_TILE_W && h == BG_TILE_H) {
                    for (uint32_t c = 4u; c <= 6u; c++) {
                        uint32_t v = psx_mod_read_byte(p + c);
                        psx_mod_write_byte(p + c,
                                           (uint8_t)(v * (uint32_t)percent / 100u));
                    }
                }
            }
        }
        p += (len + 1u) * 4u;
    }
}

/*
 * Repaint the primitives one draw_text call just produced.
 *
 * The bump allocator at [ctx+156] moves only forward, so the range between its
 * value before and after the call is exactly this line's glyphs. Walk it by
 * the length byte at +3 (words minus one) and set the colour bytes.
 *
 * Length 1 is skipped deliberately: func_8001FC6C appends a one-word draw-mode
 * command (0xE1......) whose +4 is not a colour, and painting over it would
 * corrupt the texture page rather than tint anything.
 */
static void recolor_prims(uint32_t from, uint32_t to,
                          uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t p = from;
    for (int guard = 0; p < to && guard < 1024; guard++) {
        uint32_t len = psx_mod_read_byte(p + 3u);
        if (len == 0u) break;                 /* not a tag: stop rather than walk into data */
        if (len >= 3u) {                      /* a sprite, not the draw-mode command */
            psx_mod_write_byte(p + 4u, r);
            psx_mod_write_byte(p + 5u, g);
            psx_mod_write_byte(p + 6u, b);
        }
        p += (len + 1u) * 4u;
    }
}

static void sel_color(uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* Read every frame rather than caching: the player can move these sliders
     * with the menu on screen, and a cached value would look broken. */
    *r = (uint8_t)option_int("text", "sel_red",   MENU_SEL_R);
    *g = (uint8_t)option_int("text", "sel_green", MENU_SEL_G);
    *b = (uint8_t)option_int("text", "sel_blue",  MENU_SEL_B);

    const char *e = getenv("PSX_MENU_SEL_RGB");   /* still handy for testing */
    if (e && e[0]) {
        unsigned a = 0, c = 0, d = 0;
        if (sscanf(e, "%u,%u,%u", &a, &c, &d) == 3) {
            *r = (uint8_t)(a > 255 ? 255 : a);
            *g = (uint8_t)(c > 255 ? 255 : c);
            *b = (uint8_t)(d > 255 ? 255 : d);
        }
    }
}

/* Draw one line, tinting it if it is the selected one. */
static void draw_item(CPUState *cpu, int i, int y, int sel)
{
    uint32_t ctx    = psx_mod_read_word(MENU_PADPTR);
    uint32_t before = ctx ? psx_mod_read_word(ctx + PRIM_PTR_OFF) : 0u;

    draw_text(cpu, label_x(i), y, label_string(i), MENU_EXIT_FONT);

    if (!sel || !ctx) return;
    uint32_t after = psx_mod_read_word(ctx + PRIM_PTR_OFF);
    if (after <= before) return;
    uint8_t r, g, b; sel_color(&r, &g, &b);
    recolor_prims(before, after, r, g, b);
}

static uint32_t label_string(int i)
{
    if (!s_labels_done) {
        for (int k = 0; k < MENU_LABEL_COUNT; k++) {
            uint32_t a = MENU_STR_ADDR + (uint32_t)k * MENU_LABEL_SLOT;
            const char *t = MENU_LABELS[k];
            uint32_t j = 0;
            for (; t[j] && j < MENU_LABEL_SLOT - 1u; j++)
                psx_mod_write_byte(a + j, (uint8_t)t[j]);
            psx_mod_write_byte(a + j, 0u);
        }
        s_labels_done = 1;
    }
    return MENU_STR_ADDR + (uint32_t)i * MENU_LABEL_SLOT;
}

/* The generic font is 8 pixels per glyph and the drawer's X is an offset from
 * the screen centre, so a centred line starts at minus half its width. */
static int label_x(int i)
{
    int n = 0;
    while (MENU_LABELS[i][n]) n++;
    return -(n * 8) / 2;
}

/* Blank the sprite: width and height to zero leaves the record intact -- the
 * drawer still writes its brightness and bounce counter into it, harmlessly --
 * so PSX_MENU_SPRITES=1 restores the art without a restart path. */
static void hide_sprites(void)
{
    if (s_sprites_hidden) return;
    for (uint32_t i = 0; i < MENU_ITEMS; i++) {
        uint32_t rec = psx_mod_read_word(MENU_TABLE + i * MENU_STRIDE + 8u);
        if (!in_ram(rec)) continue;
        psx_mod_write_half(rec + 4u, 0);   /* w */
        psx_mod_write_half(rec + 6u, 0);   /* h */
    }
    s_sprites_hidden = 1;
}

/*
 * Call a guest routine as a nested call -- the same contract the BIOS HLE uses
 * for event callbacks. Registers are saved and restored because the caller has
 * not spilled its own yet: this runs at a function's ENTRY.
 */
static void guest_call(CPUState *cpu, uint32_t func, uint32_t ret,
                       uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    extern void psx_dispatch_call(CPUState *cpu, uint32_t addr, uint32_t ret);
    if (!cpu || s_exit_in_call) return;

    uint32_t gpr[32];
    memcpy(gpr, cpu->gpr, sizeof gpr);
    uint32_t pc = cpu->pc, hi = cpu->hi, lo = cpu->lo;

    cpu->gpr[4]  = a0;
    cpu->gpr[5]  = a1;
    cpu->gpr[6]  = a2;
    cpu->gpr[7]  = a3;
    cpu->gpr[31] = ret;
    cpu->pc      = 0;

    s_exit_in_call = 1;
    psx_dispatch_call(cpu, func, ret);
    s_exit_in_call = 0;

    memcpy(cpu->gpr, gpr, sizeof gpr);
    cpu->pc = pc; cpu->hi = hi; cpu->lo = lo;
}

static void draw_text(CPUState *cpu, int x, int y, uint32_t str, uint32_t font)
{
    guest_call(cpu, MENU_TEXT_FUNC, MENU_TEXT_RET,
               (uint32_t)(int32_t)x, (uint32_t)(int32_t)y, str, font);
}

/*
 * The menu's own sounds. The game plays these itself right after it moves the
 * cursor -- but our navigation eats the direction before the game ever reads
 * it, so the game never gets there and the move lands silently. Playing them
 * here is not decoration: a menu where some moves click and others do not
 * reads as broken.
 */
static void menu_sfx(CPUState *cpu, uint32_t id)
{
    guest_call(cpu, MENU_SFX_FUNC, MENU_TEXT_RET, id, 0, 0, 0);
}

/* Entry of the mode-select tick: navigation, and the confirm that quits. */
static void on_menu_screen(CPUState *cpu, uint32_t addr)
{
    (void)addr;
    if (!cpu || !s_feat_extended) return;
    if (!ring_is_ours()) {
        menu_link_ring();
        /* The screen was re-entered, so the sprite records may have been
         * restored with it, and a stale selection would point at an item the
         * player never chose. Start the screen clean. */
        s_sprites_hidden = 0;
        s_exit_sel       = 0;
        s_menu_linked    = 1;
    }

    uint32_t st = psx_mod_read_word(MENU_PADPTR);
    if (!st) return;

    uint32_t np      = pad_read(st);
    uint32_t cursor  = psx_mod_read_word(MENU_CURSOR);
    int      up      = (np & MENU_UP_MASK)   != 0;
    int      down    = (np & MENU_DOWN_MASK) != 0;
    int      confirm = (np & MENU_CONFIRM)   != 0;

    if (!s_exit_sel) {
        /* Both ends of the ring reach the virtual item. Down from the last
         * real entry is the obvious one; up from the FIRST is the one that is
         * easy to forget, because the game's own ring still wraps 0 -> last
         * REAL entry and lands on INFORMATION, silently skipping EXIT GAME. */
        if (cursor == MENU_ITEMS - 1u && down) {   /* down from INFORMATION */
            s_exit_sel = 1;
            pad_eat(st, MENU_NAV_MASK | MENU_CONFIRM);
            menu_sfx(cpu, MENU_SFX_MOVE);
        } else if (cursor == 0u && up) {           /* up from the first item */
            s_exit_sel = 1;
            pad_eat(st, MENU_NAV_MASK | MENU_CONFIRM);
            menu_sfx(cpu, MENU_SFX_MOVE);
        }
        /* Leaving the virtual item sets the cursor explicitly, both ways.
         * The game's cursor sits wherever it was while EXIT GAME is selected,
         * so "just clear the flag" walks back to where you came FROM: enter
         * from ARCADE and up would take you to ARCADE again. Where a move
         * lands must depend on the direction, not on the history. */
    } else if (up) {
        s_exit_sel = 0;
        psx_mod_write_word(MENU_CURSOR, MENU_ITEMS - 1u);   /* INFORMATION */
        pad_eat(st, MENU_NAV_MASK | MENU_CONFIRM);
        menu_sfx(cpu, MENU_SFX_MOVE);
    } else if (down) {
        s_exit_sel = 0;
        psx_mod_write_word(MENU_CURSOR, 0u);                /* wrap to top */
        pad_eat(st, MENU_NAV_MASK | MENU_CONFIRM);
        menu_sfx(cpu, MENU_SFX_MOVE);
    } else if (confirm) {
        extern void psx_request_quit(void);
        pad_eat(st, MENU_CONFIRM);
        menu_sfx(cpu, MENU_SFX_ENTER);
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

    const char *sprites = getenv("PSX_MENU_SPRITES");
    int text_mode = s_feat_text && !(sprites && sprites[0] && sprites[0] != '0');
    /* Without the extended feature there is no EXIT GAME line to draw. */
    int count = s_feat_extended ? MENU_LABEL_COUNT : (int)MENU_ITEMS;
    if (!text_mode && !s_feat_extended) return;

    if (!text_mode) {
        /* Original art: only EXIT GAME is ours, and only it needs drawing. */
        int y = (int)(MENU_Y0 + MENU_ITEMS * MENU_YSTEP) - MENU_TEXT_OFFY;
        draw_item(cpu, (int)MENU_ITEMS, y, s_exit_sel);
        if (s_exit_sel) cpu->gpr[5] = 0xFFu;
        return;
    }

    hide_sprites();

    uint32_t cursor = psx_mod_read_word(MENU_CURSOR);
    for (int i = 0; i < count; i++) {
        int y   = (int)(MENU_Y0 + (uint32_t)i * MENU_YSTEP) - MENU_TEXT_OFFY;
        int sel = s_exit_sel ? (i == count - 1)
                             : ((uint32_t)i == cursor);
        draw_item(cpu, i, y, sel);
    }

    /* The game's highlight has nothing left to light up, and its bounce would
     * only move an invisible sprite. Take it away from every item. */
    cpu->gpr[5] = 0xFFu;
}


/* ── The freeze ──────────────────────────────────────────────────────────────
 *
 * Traced end to end during bring-up; the full chain is in BACKLOG.md. Short
 * version: an out-of-range animation index yields a pointer that is not an
 * animation, the evaluator then returns offset 0 for body part 15, part 15 is
 * drawn twice into the same ordering-table bucket, and since insertion is
 * "prim->next = OT[b]; OT[b] = prim" the second pass closes the chain into a
 * cycle the display-list merge walks forever.
 *
 * Two containments, at two layers. Neither is a cure.
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
        /* Past every label slot in the same zero run. */
        s_scratch = MENU_STR_ADDR + 0x200u;
        psx_mod_write_word(s_scratch + 8u,  s_scratch + 0x20u);
        psx_mod_write_word(s_scratch + 12u, s_scratch + 0x20u);
        psx_mod_write_word(s_scratch + 0x20u, 0u);
        psx_mod_write_word(s_scratch + 0x24u, 0u);   /* object count = 0 */
    }
    return s_scratch;
}

/* ── BGA dark: the 3D scene, dimmed instead of removed ──────────────────────
 *
 * Same scope as the "hide" feature above -- everything func_80022268 draws,
 * which is the whole 3D scene (17 entries per frame), never the interface.
 * The life bar, judge, combo, score, the arrows and the sequence zone are
 * emitted by other routines and are not touched here.
 *
 * The emitter bump-allocates from [ctx+156], so the primitives one call
 * produces occupy exactly the range between the cursor at its entry and the
 * cursor at the next entry. Recording the cursor at every entry and scaling
 * the previous range gives per-call precision without a return hook; the last
 * call of the frame is flushed when the ordering table is consumed.
 *
 * Scaling is multiplicative on the primitive colour (128 is neutral on a
 * textured primitive), so brightness 0 is solid black and 100 is stock. The
 * geometry still draws, which is the point: it keeps covering the fixed
 * background exactly as before, only dark -- a BGA-off look rather than a
 * hole in the scene.
 */
static uint32_t s_bga_mark;
static int      s_bga_pct = 50;

static uint32_t prim_cursor(void)
{
    uint32_t ctx = psx_mod_read_word(MENU_PADPTR);
    if (!ctx) return 0u;
    uint32_t cur = psx_mod_read_word(ctx + PRIM_PTR_OFF);
    return in_ram(cur) ? cur : 0u;
}

/* Gouraud primitives carry one colour per vertex; flat and textured-flat ones
 * carry a single colour in the command word. The command byte tells which:
 * 0x10 = gouraud, 0x08 = quad, 0x04 = textured. */
static void scale_prims(uint32_t from, uint32_t to, int percent)
{
    if (percent >= 100 || !from || !to || to <= from) return;
    if (percent < 0) percent = 0;

    uint32_t p = from;
    for (int guard = 0; p < to && guard < 4096; guard++) {
        uint32_t len = psx_mod_read_byte(p + 3u);
        if (len == 0u) break;                 /* not a tag: stop, do not walk into data */
        uint32_t end = p + (len + 1u) * 4u;
        if (len >= 3u) {
            uint32_t cmd    = psx_mod_read_byte(p + 7u);
            uint32_t nverts = 1u, stride = 0u;
            if (cmd >= 0x20u && cmd < 0x40u && (cmd & 0x10u)) {
                nverts = (cmd & 0x08u) ? 4u : 3u;
                stride = (cmd & 0x04u) ? 3u : 2u;   /* colour + xy [+ uv], in words */
            }
            for (uint32_t v = 0; v < nverts; v++) {
                uint32_t c = p + 4u + v * stride * 4u;
                if (c + 3u > end) break;
                for (uint32_t k = 0; k < 3u; k++) {
                    uint32_t val = psx_mod_read_byte(c + k);
                    psx_mod_write_byte(c + k, (uint8_t)(val * (uint32_t)percent / 100u));
                }
            }
        }
        p = end;
    }
}

static void bga_flush(void)
{
    uint32_t cur = prim_cursor();
    if (s_bga_mark && cur > s_bga_mark) scale_prims(s_bga_mark, cur, s_bga_pct);
    s_bga_mark = cur;
}

/* ── Frame-rate probe (PSX_FPS=1) ────────────────────────────────────────────
 *
 * func_8007259C consumes the ordering table exactly once per drawn frame, so
 * counting its entries against the host clock IS the guest frame rate -- no
 * host-side present counter, which can double-present or drop. The model
 * emitter's call count comes along because it says how much 3D the frame that
 * cost that much was carrying. */
static void fps_tick(int is_frame)
{
    static int   enabled = -1;
    static long  frames, models;
    static time_t t0;

    if (enabled < 0) { const char *e = getenv("PSX_FPS"); enabled = (e && *e && *e != '0'); }
    if (!enabled) return;

    if (is_frame) frames++; else models++;
    if (!is_frame) return;

    time_t now = time(NULL);
    if (!t0) { t0 = now; return; }
    if (now == t0) return;
    fprintf(stderr, "ddr: fps=%ld  model_calls=%ld (%.1f per frame)\n",
            frames, models, frames ? (double)models / (double)frames : 0.0);
    fflush(stderr);
    t0 = now; frames = 0; models = 0;
}

static void on_model_emit(CPUState *cpu, uint32_t addr)
{
    (void)addr;
    if (!cpu) return;

    /* Hiding the dancer is the same mechanism as suppressing a duplicate:
     * hand the emitter an empty object list. No code is patched, and the rest
     * of the scene is untouched. */
    fps_tick(0);
    if (s_feat_bga_dark) bga_flush();

    if (s_feat_no_dancer) { cpu->gpr[4] = dedup_scratch(); return; }

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
/* Reports the cadence byte whenever it changes, and -- with PSX_VSYNC=n --
 * holds it at n. PSX_VSYNC=1 asks the loop for one field instead of two.
 * Whether the game SURVIVES that is the open question: if the chart steps once
 * per loop turn, 60 Hz plays the song at double speed. This is the experiment,
 * not the feature. */
static void vsync_force(void)
{
    static int     mode = -2;
    static uint8_t last = 0xFFu;

    if (mode == -2) { const char *e = getenv("PSX_VSYNC"); mode = (e && *e) ? atoi(e) : -1; }

    int want = (mode > 0) ? mode : (s_feat_fps60 ? 1 : -1);

    uint8_t cur = psx_mod_read_byte(VSYNC_MODE_ADDR);
    if (cur != last) {
        fprintf(stderr, "ddr: vsync mode = %u (%s)\n", cur,
                cur == 1u ? "60 Hz" : (cur == 2u ? "30 Hz" : "?"));
        fflush(stderr);
        last = cur;
    }
    if (want > 0 && cur != (uint8_t)want) {
        psx_mod_write_byte(VSYNC_MODE_ADDR, (uint8_t)want);
        last = (uint8_t)want;
    }
}

static void on_ot_merge(CPUState *cpu, uint32_t addr)
{
    (void)cpu; (void)addr;

    /* The list has just been consumed: a primitive repeated after this point
     * belongs to a new frame, not to a duplicate. This is the real interval,
     * and using it instead of a host frame counter is what makes the check
     * honest -- a dropped present would otherwise read as a duplicate. */
    s_seen_n = 0;
    fps_tick(1);
    vsync_force();

    if (s_feat_bga_dark) {
        bga_flush();                 /* the last call of the frame */
        s_bga_mark = 0;
        s_bga_pct  = option_int_pkg("ddr.bga", "dark", "brightness", 50);
    }

    if (s_feat_dim_bg)
        dim_background(option_int_pkg("ddr.background", "dim", "brightness", 50));

    if (!s_feat_unlocks || !s_menu_linked) return;
    uint32_t cur = psx_mod_read_word(UNLOCK_MASK_ADDR);
    if ((cur & UNLOCK_MASK_ALL) != UNLOCK_MASK_ALL)
        psx_mod_write_word(UNLOCK_MASK_ADDR, cur | UNLOCK_MASK_ALL);
    uint8_t b = psx_mod_read_byte(UNLOCK_SONG_ADDR);
    if (!(b & 1u)) psx_mod_write_byte(UNLOCK_SONG_ADDR, (uint8_t)(b | 1u));
}

/* ── Registration ───────────────────────────────────────────────────────────
 * Addresses must also be listed in game.toml [recompiler]
 * mod_function_entry_funcs, or these callbacks are never reached. */
PSX_MOD_CONSTRUCTOR(ddr_register_hooks)
{
    (void)psx_mod_register_activation_plugin("ddr.menu.extended", ddr_activate_extended);
    (void)psx_mod_register_activation_plugin("ddr.menu.text",     ddr_activate_text);
    (void)psx_mod_register_activation_plugin("ddr.unlocks.all",   ddr_activate_unlocks);
    (void)psx_mod_register_activation_plugin("ddr.dancer.hide",   ddr_activate_no_dancer);
    (void)psx_mod_register_activation_plugin("ddr.background.dim", ddr_activate_dim_bg);
    (void)psx_mod_register_activation_plugin("ddr.bga.dark",       ddr_activate_bga_dark);
    (void)psx_mod_register_activation_plugin("ddr.framerate.sixty", ddr_activate_fps60);

    (void)psx_mod_register_function_entry_plugin("ddr.menu",  DDR_MENU_SCREEN, on_menu_screen);
    (void)psx_mod_register_function_entry_plugin("ddr.menu",  DDR_MENU_DRAW,   on_menu_draw);
    (void)psx_mod_register_function_entry_plugin("ddr.dedup", DDR_MODEL_EMIT,  on_model_emit);
    (void)psx_mod_register_function_entry_plugin("ddr.dedup", DDR_OT_MERGE,    on_ot_merge);
    (void)psx_mod_register_function_entry_plugin("ddr.anim",  DDR_ANIM_DRIVER, on_anim_driver);
}
