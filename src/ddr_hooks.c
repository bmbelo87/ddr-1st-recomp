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
#include "warning_art.h"
#include "mcard_art.h"
#include "band_art.h"

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
static int      s_feat_judge;
static void ddr_activate_judge(void)    { s_feat_judge     = 1; }
static int      s_feat_warning;
static void ddr_activate_warning(void)  { s_feat_warning   = 1; }

/* Which artwork to upload. The language is a choice option rather than a second
 * feature because the two are mutually exclusive -- the mod format asks for
 * exactly that. Read fresh at each replacement, so changing it in the launcher
 * takes effect the next time a screen appears. */
static int lang_is_pt(void)
{
    char buf[16];
    if (psx_mod_option_value("ddr.warning", "translate", "language", buf, sizeof buf) &&
        (buf[0] == 'p' || buf[0] == 'P'))
        return 1;
    return 0;
}
static int      s_feat_menu_bd;
static void ddr_activate_menu_bd(void)  { s_feat_menu_bd   = 1; }
static int      s_feat_fps60;
static void ddr_activate_fps60(void)    { s_feat_fps60     = 1; }

/* Widescreen is the framework's, not ours: the activation callback runs before
 * the renderer exists, which is the one moment a fixed aspect can be chosen.
 * The GTE squash plus the stretched present widens the field of view of the 3D
 * stage; [widescreen] hud_sprt_squash / auto_ui_squash in game.toml put the 2D
 * back at its own proportions so the arrows stay round. */
static void ddr_activate_widescreen(void)
{
    (void)psx_mod_set_fixed_display_aspect(16u, 9u);
}

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

/* ── Chart/music offset ─────────────────────────────────────────────────────
 *
 * The chart starts counting on the CD Play ACK, and the game was tuned for a
 * real drive: on hardware the first audio frame is not audible until the disc
 * has settled, so the music arrives a little AFTER the chart begins. The
 * emulated drive starts instantly, so the music arrives early relative to the
 * chart, and a player stepping to what they hear steps early -- which reads as
 * GREAT on a step that felt perfect.
 *
 * runtime/src/cdrom.c already supports restoring that settle, as an opt-in
 * environment variable read lazily at the first Play. Activation happens before
 * the game boots, so setting it here is enough, and it keeps the value where a
 * player can actually reach it instead of in a config file.
 *
 * This only shifts one way -- it can make the music later, never earlier. The
 * opposite direction is the audio output cushion ([audio] buffer_ms in
 * game.toml, 60 ms here), which is what delays what you HEAR relative to the
 * emulated timeline. The two errors have opposite signs, which is why the
 * result feels inconsistent rather than simply late.
 */
static void ddr_activate_timing(void)
{
    int ms = option_int_pkg("ddr.timing", "offset", "music_delay_ms", 0);
    char buf[16];
    snprintf(buf, sizeof buf, "%d", ms);
#ifdef _WIN32
    _putenv_s("PSX_CDDA_PLAY_DELAY_MS", buf);
#else
    setenv("PSX_CDDA_PLAY_DELAY_MS", buf, 1);
#endif
    fprintf(stderr, "ddr: music offset = %d ms\n", ms);
    fflush(stderr);
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

/* ── Big-primitive dump (PSX_PRIMDUMP=1) ────────────────────────────────────
 *
 * Walks the whole primitive buffer at the ordering-table merge and reports the
 * ones that cover most of the screen, with their command byte, colour and
 * extent. That is how a full-screen layer is identified without guessing:
 * whatever paints the menu white has to be in here, and it has to be wide.
 * Prints one burst every 120 frames so the console stays readable. */
static void prim_dump(void)
{
    static int enabled = -1;
    static uint32_t frame;

    if (enabled < 0) { const char *e = getenv("PSX_PRIMDUMP"); enabled = (e && *e) ? atoi(e) : 0; }
    if (!enabled) return;
    if (frame++ % (enabled >= 2 ? 20u : 120u)) return;   /* level 2 chases short screens */

    uint32_t ctx = psx_mod_read_word(MENU_PADPTR);
    if (!ctx) return;
    uint32_t cur = psx_mod_read_word(ctx + PRIM_PTR_OFF);
    uint32_t s0  = psx_mod_read_word(PRIM_STARTS);
    uint32_t s1  = psx_mod_read_word(PRIM_STARTS + 4u);
    if (!in_ram(cur) || !in_ram(s0) || !in_ram(s1)) return;
    uint32_t p = (cur >= s1 && s1 > s0) ? s1 : ((cur >= s0) ? s0 : 0u);
    if (!p) return;

    fprintf(stderr, "ddr: --- prim dump (buffer %08X..%08X) ---\n", p, cur);
    uint32_t page_now = 0u;   /* a rectangle has no page of its own: it uses
                               * whatever the last draw-mode command set. */
    for (int guard = 0; p < cur && guard < 4096; guard++) {
        uint32_t len = psx_mod_read_byte(p + 3u);
        if (len == 0u) break;
        uint32_t end = p + (len + 1u) * 4u;
        if (len == 1u) {
            uint32_t word = psx_mod_read_word(p + 4u);
            if ((word >> 24) == 0xE1u) page_now = word & 0x7FFu;
            p = end; continue;
        }
        if (len < 3u) { p = end; continue; }

        uint32_t cmd = psx_mod_read_byte(p + 7u);
        int r = psx_mod_read_byte(p + 4u);
        int g = psx_mod_read_byte(p + 5u);
        int b = psx_mod_read_byte(p + 6u);
        int x0 = 0x7FFF, y0 = 0x7FFF, x1 = -0x7FFF, y1 = -0x7FFF;
        const char *kind = "?";

        if (cmd >= 0x20u && cmd < 0x40u) {            /* polygon */
            uint32_t tex = (cmd & 0x04u) ? 1u : 0u;
            uint32_t gou = (cmd & 0x10u) ? 1u : 0u;
            uint32_t nv  = (cmd & 0x08u) ? 4u : 3u;
            uint32_t w   = 1u;                         /* word index, 0 = tag */
            kind = "poly";
            for (uint32_t v = 0; v < nv; v++) {
                if (v == 0u || gou) w++;               /* colour word */
                uint32_t a = p + w * 4u;
                if (a + 4u > end) break;
                int x = (int16_t)psx_mod_read_half(a);
                int y = (int16_t)psx_mod_read_half(a + 2u);
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
                w++;
                if (tex) w++;
            }
        } else if (cmd >= 0x60u && cmd < 0x80u) {      /* rectangle / sprite */
            uint32_t tex = (cmd & 0x04u) ? 1u : 0u;
            uint32_t sz  = (cmd >> 3) & 3u;
            uint32_t w   = 2u;
            kind = "rect";
            x0 = (int16_t)psx_mod_read_half(p + w * 4u);
            y0 = (int16_t)psx_mod_read_half(p + w * 4u + 2u);
            w++;
            if (tex) w++;
            int ww = (sz == 1u) ? 1 : (sz == 2u) ? 8 : (sz == 3u) ? 16 : 0;
            int hh = ww;
            if (sz == 0u && p + w * 4u + 4u <= end) {
                ww = (uint16_t)psx_mod_read_half(p + w * 4u);
                hh = (uint16_t)psx_mod_read_half(p + w * 4u + 2u);
            }
            x1 = x0 + ww; y1 = y0 + hh;
        } else {
            p = end; continue;
        }

        int w = x1 - x0, h = y1 - y0;
        /* Level 1: only the full-screen layers. Level 2: everything, with the
         * texture words, which is what identifies where in VRAM an image is
         * and whether the screen is one picture or a hundred glyphs. */
        if (enabled >= 2 || w >= 200 || h >= 180) {
            char tex[64];
            tex[0] = 0;
            if (cmd & 0x04u) {
                uint32_t uvclut = psx_mod_read_word(p + 12u);   /* uv + clut */
                /* Textured quad: tag, C0, XY0, UV0|CLUT, C1, XY1, UV1|TPAGE, ...
                 * so the page lives in the upper half of word 6. A rect has no
                 * page of its own -- it uses whatever E1 last set. */
                uint32_t tpage = (cmd >= 0x60u) ? page_now : psx_mod_read_half(p + 26u);
                snprintf(tex, sizeof tex, " uv=%02X,%02X clut=%04X tpage=%04X",
                         uvclut & 0xFFu, (uvclut >> 8) & 0xFFu,
                         (uvclut >> 16) & 0xFFFFu, tpage);
            }
            fprintf(stderr, "ddr:  %08X cmd=%02X %s rgb=%3d,%3d,%3d  %dx%d at %d,%d%s\n",
                    p, cmd, kind, r, g, b, w, h, x0, y0, tex);
        }
        p = end;
    }
    fflush(stderr);
}

/* ── Menu backdrop ──────────────────────────────────────────────────────────
 *
 * The mode-select screen lays a full-screen untextured rectangle over the
 * scene: 320x240 from the corner, colour 160,160,160, command 0x62 -- the
 * 0x02 bit is semi-transparency. Measured on screen, its blend mode is ADD:
 * it adds grey to everything behind it, which is the wash. Re-colouring it
 * therefore cannot darken anything -- black is the neutral element of an
 * addition, so 0 simply makes it vanish.
 *
 * The mode that does darken is SUBTRACT (back - front), and the mode is not a
 * property of the primitive: it lives in the draw-mode command (0xE1) that
 * preceded it. That command is in the same buffer, so the fix is a surgical
 * edit of the display list:
 *
 *   the rectangle's packet  ->  becomes a 1-word 0xE1 (the same texpage the
 *                               game set, with the blend bits switched to
 *                               subtract), chained to
 *   scratch: the rectangle   -> the original rect, re-coloured to the level
 *   scratch: restore 0xE1    -> the game's own draw-mode word, put back so
 *                               everything drawn after is untouched
 *   -> the packet's original next
 *
 * Nothing is inserted ahead of the rectangle, so its predecessor never has to
 * be found: the packet is rewritten in place and the rest is chained behind
 * it. The buffer is rebuilt from scratch every frame, so none of this
 * accumulates.
 */
#define MENU_BD_CMD   0x62u
#define MENU_BD_W_LO  300
#define MENU_BD_W_HI  340
#define MENU_BD_H_LO  220
#define MENU_BD_H_HI  260
#define E1_MODE_MASK  0x00000060u   /* bits 5-6: 0 = B/2+F/2, 1 = B+F, 2 = B-F */
#define E1_MODE_SUB   0x00000040u

static uint32_t bd_scratch(void)
{
    static uint32_t base;
    if (!base) base = MENU_STR_ADDR + 0x300u;   /* past the label slots and the dedup struct */
    return base;
}

static uint32_t prim_tag(uint32_t next, uint32_t words)
{
    return ((next & 0x00FFFFFFu) | (words << 24));
}

static void menu_backdrop(int percent)
{
    if (percent <= 0) return;
    if (percent > 100) percent = 100;
    uint32_t level = (uint32_t)percent * 255u / 100u;

    uint32_t ctx = psx_mod_read_word(MENU_PADPTR);
    if (!ctx) return;
    uint32_t cur = psx_mod_read_word(ctx + PRIM_PTR_OFF);
    uint32_t s0  = psx_mod_read_word(PRIM_STARTS);
    uint32_t s1  = psx_mod_read_word(PRIM_STARTS + 4u);
    if (!in_ram(cur) || !in_ram(s0) || !in_ram(s1)) return;
    uint32_t p = (cur >= s1 && s1 > s0) ? s1 : ((cur >= s0) ? s0 : 0u);
    if (!p) return;

    uint32_t last_e1 = 0u;      /* the draw-mode word in force when the rect is reached */

    for (int guard = 0; p < cur && guard < 4096; guard++) {
        uint32_t len = psx_mod_read_byte(p + 3u);
        if (len == 0u) break;
        uint32_t end  = p + (len + 1u) * 4u;
        uint32_t word = psx_mod_read_word(p + 4u);

        if (len == 1u && (word >> 24) == 0xE1u) { last_e1 = word; p = end; continue; }

        if (len >= 3u && (word >> 24) == MENU_BD_CMD && p + 16u <= end) {
            int w = (uint16_t)psx_mod_read_half(p + 12u);
            int h = (uint16_t)psx_mod_read_half(p + 14u);
            if (w >= MENU_BD_W_LO && w <= MENU_BD_W_HI &&
                h >= MENU_BD_H_LO && h <= MENU_BD_H_HI && last_e1) {
                uint32_t nxt = psx_mod_read_word(p) & 0x00FFFFFFu;
                uint32_t xy  = psx_mod_read_word(p + 8u);
                uint32_t wh  = psx_mod_read_word(p + 12u);
                uint32_t sc  = bd_scratch();

                /* the rectangle, re-coloured, in scratch */
                psx_mod_write_word(sc,        prim_tag(sc + 0x20u, 3u));
                psx_mod_write_word(sc + 4u,   (MENU_BD_CMD << 24) |
                                              (level << 16) | (level << 8) | level);
                psx_mod_write_word(sc + 8u,   xy);
                psx_mod_write_word(sc + 12u,  wh);

                /* put the game's own draw mode back */
                psx_mod_write_word(sc + 0x20u, prim_tag(nxt, 1u));
                psx_mod_write_word(sc + 0x24u, last_e1);

                /* the packet in place becomes the subtract draw-mode command */
                psx_mod_write_word(p,        prim_tag(sc, 1u));
                psx_mod_write_word(p + 4u,   (last_e1 & ~E1_MODE_MASK) | E1_MODE_SUB);
                return;                      /* one backdrop per frame */
            }
        }
        p = end;
    }
}

/* ── TIM finder (PSX_TIMDUMP=1) ─────────────────────────────────────────────
 *
 * A TIM is Sony's texture file, and the game loads it from the disc into RAM
 * before handing it to the GPU, so between those two moments the picture is
 * just bytes a hook can read -- and, later, write. The header is rigid enough
 * to scan for: 0x00000010, then flags whose low three bits are the pixel mode
 * (0 = 4bpp, 1 = 8bpp, 2 = 16bpp) and whose bit 3 says a CLUT block follows.
 * Each block carries its own byte count and its VRAM x/y/w/h, which is what
 * lets a find be matched against the clut/tpage the draw call used.
 *
 * Every hit is written out as a PPM next to the executable. The hook runs in
 * the host process, so writing a file is just fopen -- no debug build, no
 * protocol, nothing to keep running.
 */
static void tim_write_ppm(const char *path, int w, int h, uint32_t px_addr,
                          int bpp, uint32_t clut_addr, int clut_n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "ddr: tim: cannot write %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint16_t c;
            if (bpp == 16) {
                c = psx_mod_read_half(px_addr + (uint32_t)(y * w + x) * 2u);
            } else {
                uint32_t i;
                uint8_t idx;
                if (bpp == 4) {
                    i = (uint32_t)y * (uint32_t)w / 2u + (uint32_t)x / 2u;
                    uint8_t byte = psx_mod_read_byte(px_addr + i);
                    idx = (x & 1) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0Fu);
                } else {
                    i = (uint32_t)y * (uint32_t)w + (uint32_t)x;
                    idx = psx_mod_read_byte(px_addr + i);
                }
                if (clut_n && idx >= clut_n) idx = 0;
                c = psx_mod_read_half(clut_addr + (uint32_t)idx * 2u);
            }
            /* BGR555 -> RGB888 */
            unsigned char rgb[3];
            rgb[0] = (unsigned char)(((c      ) & 0x1Fu) << 3);
            rgb[1] = (unsigned char)(((c >>  5) & 0x1Fu) << 3);
            rgb[2] = (unsigned char)(((c >> 10) & 0x1Fu) << 3);
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

#define TIM_SEEN_MAX 32

static void tim_scan(void)
{
    static int      enabled = -1;
    static uint32_t frame;
    static uint32_t seen[TIM_SEEN_MAX];
    static int      seen_n;

    if (enabled < 0) { const char *e = getenv("PSX_TIMDUMP"); enabled = (e && *e && *e != '0'); }
    if (!enabled) return;
    if (frame++ % 300u) return;                 /* every ~5 s, so screens can be visited */

    fprintf(stderr, "ddr: --- TIM scan ---\n");
    for (uint32_t a = 0x80010000u; a < 0x801F0000u; a += 4u) {
        if (psx_mod_read_word(a) != 0x00000010u) continue;
        uint32_t flags = psx_mod_read_word(a + 4u);
        if (flags & ~0x0000000Fu) continue;
        uint32_t mode = flags & 7u;
        if (mode > 2u) continue;
        int bpp = (mode == 0u) ? 4 : (mode == 1u) ? 8 : 16;
        int has_clut = (flags & 8u) ? 1 : 0;
        if ((bpp == 16) == has_clut) continue;  /* clut iff indexed */

        uint32_t off = a + 8u;
        uint32_t clut_addr = 0u;
        int clut_n = 0;
        if (has_clut) {
            uint32_t blen = psx_mod_read_word(off);
            uint32_t cw   = psx_mod_read_half(off + 8u);
            uint32_t ch   = psx_mod_read_half(off + 10u);
            if (blen < 12u || blen > 0x20000u || !cw || cw > 256u || !ch || ch > 256u) continue;
            clut_addr = off + 12u;
            clut_n    = (int)cw;
            off      += blen;
        }
        uint32_t blen = psx_mod_read_word(off);
        uint32_t vx   = psx_mod_read_half(off + 4u);
        uint32_t vy   = psx_mod_read_half(off + 6u);
        uint32_t hw   = psx_mod_read_half(off + 8u);   /* width in VRAM halfwords */
        uint32_t hh   = psx_mod_read_half(off + 10u);
        if (blen < 12u || blen > 0x100000u || !hw || !hh || hw > 1024u || hh > 512u) continue;

        uint32_t w = (bpp == 4) ? hw * 4u : (bpp == 8) ? hw * 2u : hw;
        int dup = 0;
        for (int i = 0; i < seen_n; i++) if (seen[i] == a) dup = 1;
        fprintf(stderr, "ddr:  TIM @%08X  %ubpp %ux%u  vram=%u,%u  clut=%d entries%s\n",
                a, (unsigned)bpp, (unsigned)w, (unsigned)hh,
                (unsigned)vx, (unsigned)vy, clut_n, dup ? "  (already written)" : "");
        if (dup || seen_n >= TIM_SEEN_MAX) continue;
        seen[seen_n++] = a;

        char path[128];
        snprintf(path, sizeof path, "tim_%08X_%ux%u.ppm", a, (unsigned)w, (unsigned)hh);
        tim_write_ppm(path, (int)w, (int)hh, off + 12u, bpp, clut_addr, clut_n);
        fprintf(stderr, "ddr:   -> %s\n", path);
    }
    fflush(stderr);
}

/* ── Texture dump (PSX_TEXDUMP=1) ───────────────────────────────────────────
 *
 * The RAM copy of a picture is transient -- the game reuses that buffer as
 * soon as the upload is done, which is why a scan five seconds later finds the
 * next screen's art instead. VRAM is not transient: while the image is on
 * screen it is sitting in the frame store, and gpu_vram_peek reads it.
 *
 * So instead of hunting a file, this follows the draw calls: every textured
 * primitive names a texture page and a palette, and each distinct pair is
 * decoded once and written out. Whatever is on screen gets dumped, whatever
 * its provenance.
 *
 * Page layout: tpage bits 0-3 are X/64, bit 4 is Y/256, bits 7-8 the depth
 * (0 = 4bpp, 1 = 8bpp). A CLUT word is X/16 in bits 0-5 and Y in bits 6-14.
 */
extern uint16_t gpu_vram_peek(int x, int y);
extern void     gpu_write_gp0(uint32_t val);

#define TEX_SEEN_MAX 64

static void tex_write(uint32_t tpage, uint32_t clut)
{
    static int seq;
    int bpp = ((tpage >> 7) & 3u) == 0u ? 4 : (((tpage >> 7) & 3u) == 1u ? 8 : 16);
    int px  = (int)(tpage & 0x0Fu) * 64;
    int py  = (int)((tpage >> 4) & 1u) * 256;
    int cx  = (int)(clut & 0x3Fu) * 16;
    int cy  = (int)((clut >> 6) & 0x1FFu);
    int w   = (bpp == 4) ? 256 : (bpp == 8) ? 128 : 64;

    char path[128];
    snprintf(path, sizeof path, "tex_%02d_%04X_%04X_%ubpp.ppm",
             seq++, (unsigned)tpage, (unsigned)clut, (unsigned)bpp);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, 256);

    for (int v = 0; v < 256; v++) {
        for (int u = 0; u < w; u++) {
            uint16_t c;
            if (bpp == 16) {
                c = gpu_vram_peek(px + u, py + v);
            } else if (bpp == 8) {
                uint16_t hw = gpu_vram_peek(px + u / 2, py + v);
                uint8_t idx = (u & 1) ? (uint8_t)(hw >> 8) : (uint8_t)(hw & 0xFFu);
                c = gpu_vram_peek(cx + idx, cy);
            } else {
                uint16_t hw = gpu_vram_peek(px + u / 4, py + v);
                uint8_t idx = (uint8_t)((hw >> ((u & 3) * 4)) & 0x0Fu);
                c = gpu_vram_peek(cx + idx, cy);
            }
            unsigned char rgb[3];
            rgb[0] = (unsigned char)(((c      ) & 0x1Fu) << 3);
            rgb[1] = (unsigned char)(((c >>  5) & 0x1Fu) << 3);
            rgb[2] = (unsigned char)(((c >> 10) & 0x1Fu) << 3);
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    fprintf(stderr, "ddr:  texture tpage=%04X clut=%04X %ubpp  page at VRAM %d,%d "
                    "palette at %d,%d  -> %s\n",
            (unsigned)tpage, (unsigned)clut, (unsigned)bpp, px, py, cx, cy, path);
    fflush(stderr);
}

/* Level 2 keys on the SHAPE of the frame -- how many strips, how wide, where --
 * not just the page and palette. A page whose contents are rewritten between
 * screens (0x0015 carries three different messages) then dumps once per screen
 * instead of once ever. */
static void tex_dump(void)
{
    static int      enabled = -1;
    static uint32_t seen[TEX_SEEN_MAX];
    static int      seen_n;
    static uint32_t last_shape;

    if (enabled < 0) { const char *e = getenv("PSX_TEXDUMP"); enabled = (e && *e) ? atoi(e) : 0; }
    if (!enabled) return;

    uint32_t ctx = psx_mod_read_word(MENU_PADPTR);
    if (!ctx) return;
    uint32_t cur = psx_mod_read_word(ctx + PRIM_PTR_OFF);
    uint32_t s0  = psx_mod_read_word(PRIM_STARTS);
    uint32_t s1  = psx_mod_read_word(PRIM_STARTS + 4u);
    if (!in_ram(cur) || !in_ram(s0) || !in_ram(s1)) return;
    uint32_t p = (cur >= s1 && s1 > s0) ? s1 : ((cur >= s0) ? s0 : 0u);
    if (!p) return;

    /* One pass to describe the frame's textured strips, so a changed layout can
     * re-arm the dump. */
    if (enabled >= 2) {
        uint32_t shape = 0u, q = p;
        for (int guard = 0; q < cur && guard < 4096; guard++) {
            uint32_t len = psx_mod_read_byte(q + 3u);
            if (len == 0u) break;
            uint32_t cmd = psx_mod_read_byte(q + 7u);
            if (len >= 3u && (cmd & 0x04u) && cmd >= 0x20u && cmd < 0x60u) {
                int x0 = (int16_t)psx_mod_read_half(q + 8u);
                int y0 = (int16_t)psx_mod_read_half(q + 10u);
                shape = shape * 31u + (uint32_t)(x0 * 7 + y0) +
                        psx_mod_read_half(q + 26u);
            }
            q += (len + 1u) * 4u;
        }
        if (shape != last_shape) { last_shape = shape; seen_n = 0; }
    }

    for (int guard = 0; p < cur && guard < 4096; guard++) {
        uint32_t len = psx_mod_read_byte(p + 3u);
        if (len == 0u) break;
        uint32_t end = p + (len + 1u) * 4u;
        uint32_t cmd = psx_mod_read_byte(p + 7u);
        if (len >= 3u && (cmd & 0x04u) && cmd >= 0x20u && cmd < 0x60u) {
            uint32_t clut  = psx_mod_read_half(p + 14u);
            uint32_t tpage = psx_mod_read_half(p + 26u);
            uint32_t key   = (tpage << 16) | clut;
            int dup = 0;
            for (int i = 0; i < seen_n; i++) if (seen[i] == key) dup = 1;
            if (!dup && seen_n < TEX_SEEN_MAX) {
                seen[seen_n++] = key;
                tex_write(tpage, clut);
            }
        }
        p = end;
    }
}

/* ── CAUTION screen in English ──────────────────────────────────────────────
 *
 * The screen is a 256x160 picture drawn as ten textured strips of 256x16 out
 * of the page at tpage 0x000A (VRAM 640,0), 4bpp, palette 0x7C3C (VRAM
 * 960,496). Two palette entries carry the whole thing: black paper, cream ink.
 *
 * Replacing it does not mean finding the file. The RAM copy is transient -- the
 * game reuses that buffer the moment the upload finishes -- but VRAM holds the
 * picture for as long as it is on screen, and gpu_write_gp0 is exported, so a
 * real CPU-to-VRAM transfer can be issued down the same path the game uses.
 * No external file for the player to lose, and no patched game code.
 *
 * The palette indices are read, never assumed: whichever of the sixteen
 * entries is darkest becomes paper and whichever is brightest becomes ink. A
 * 4bpp VRAM halfword packs four texels, low nibble leftmost.
 */
#define WARN_TPAGE   0x000Au
#define WARN_CLUT    0x7C3Cu

static void warning_replace(void)
{
    int cx = (int)(WARN_CLUT & 0x3Fu) * 16;
    int cy = (int)((WARN_CLUT >> 6) & 0x1FFu);
    int px = (int)(WARN_TPAGE & 0x0Fu) * 64;
    int py = (int)((WARN_TPAGE >> 4) & 1u) * 256;

    int ink = 0, paper = 0, best = -1, worst = 1 << 30;
    for (int i = 0; i < 16; i++) {
        uint16_t c = gpu_vram_peek(cx + i, cy);
        int lum = (int)((c & 0x1Fu) + ((c >> 5) & 0x1Fu) + ((c >> 10) & 0x1Fu));
        if (lum > best)  { best = lum;  ink = i; }
        if (lum < worst) { worst = lum; paper = i; }
    }
    if (ink == paper) return;                      /* palette not loaded yet */

    gpu_write_gp0(0xA0000000u);                                    /* CPU -> VRAM */
    gpu_write_gp0(((uint32_t)py << 16) | (uint32_t)px);            /* destination */
    gpu_write_gp0(((uint32_t)WARNING_H << 16) | 64u);           /* 64 halfwords x 160 */

    const unsigned char *art = lang_is_pt() ? warning_bits_pt : warning_bits_en;
    for (int y = 0; y < WARNING_H; y++) {
        const unsigned char *row = art + (size_t)y * (WARNING_W / 8);
        for (int hw = 0; hw < 64; hw += 2) {                       /* two halfwords per word */
            uint32_t word = 0u;
            for (int k = 0; k < 2; k++) {
                uint32_t half = 0u;
                for (int t = 0; t < 4; t++) {
                    int x   = (hw + k) * 4 + t;
                    int set = (row[x >> 3] >> (7 - (x & 7))) & 1;
                    half |= (uint32_t)(set ? ink : paper) << (t * 4);
                }
                word |= half << (k * 16);
            }
            gpu_write_gp0(word);
        }
    }
}

/* True when this frame draws the CAUTION strips, which is the only moment the
 * picture is both present and ours to overwrite. */
static int warning_on_screen(void)
{
    uint32_t ctx = psx_mod_read_word(MENU_PADPTR);
    if (!ctx) return 0;
    uint32_t cur = psx_mod_read_word(ctx + PRIM_PTR_OFF);
    uint32_t s0  = psx_mod_read_word(PRIM_STARTS);
    uint32_t s1  = psx_mod_read_word(PRIM_STARTS + 4u);
    if (!in_ram(cur) || !in_ram(s0) || !in_ram(s1)) return 0;
    uint32_t p = (cur >= s1 && s1 > s0) ? s1 : ((cur >= s0) ? s0 : 0u);
    if (!p) return 0;

    for (int guard = 0; p < cur && guard < 4096; guard++) {
        uint32_t len = psx_mod_read_byte(p + 3u);
        if (len == 0u) break;
        uint32_t end = p + (len + 1u) * 4u;
        uint32_t cmd = psx_mod_read_byte(p + 7u);
        if (len >= 3u && (cmd & 0x04u) && cmd >= 0x20u && cmd < 0x60u &&
            psx_mod_read_half(p + 14u) == WARN_CLUT &&
            psx_mod_read_half(p + 26u) == WARN_TPAGE)
            return 1;
        p = end;
    }
    return 0;
}

/* The red bar under the title is not part of the texture: it is a separate
 * flat rectangle, 44x6, sized for the two ideograms of the original title.
 * "CAUTION" is 65 px wide, so the bar is re-cut around its own centre to match
 * -- the one measurement that has to agree with the artwork, kept next to it. */
#define WARN_RULE_W   44
#define WARN_RULE_H   6
#define WARN_TITLE_W  66

static void warning_rule(void)
{
    uint32_t ctx = psx_mod_read_word(MENU_PADPTR);
    if (!ctx) return;
    uint32_t cur = psx_mod_read_word(ctx + PRIM_PTR_OFF);
    uint32_t s0  = psx_mod_read_word(PRIM_STARTS);
    uint32_t s1  = psx_mod_read_word(PRIM_STARTS + 4u);
    if (!in_ram(cur) || !in_ram(s0) || !in_ram(s1)) return;
    uint32_t p = (cur >= s1 && s1 > s0) ? s1 : ((cur >= s0) ? s0 : 0u);
    if (!p) return;

    for (int guard = 0; p < cur && guard < 4096; guard++) {
        uint32_t len = psx_mod_read_byte(p + 3u);
        if (len == 0u) break;
        uint32_t end = p + (len + 1u) * 4u;
        if (len >= 3u && psx_mod_read_byte(p + 7u) == 0x60u && p + 16u <= end) {
            int w = (uint16_t)psx_mod_read_half(p + 12u);
            int h = (uint16_t)psx_mod_read_half(p + 14u);
            if (w == WARN_RULE_W && h == WARN_RULE_H) {
                int x  = (int16_t)psx_mod_read_half(p + 8u);
                int cx = x + WARN_RULE_W / 2;
                psx_mod_write_half(p + 8u,  (uint16_t)(int16_t)(cx - WARN_TITLE_W / 2));
                psx_mod_write_half(p + 12u, (uint16_t)WARN_TITLE_W);
                return;
            }
        }
        p = end;
    }
}

static void warning_tick(void)
{
    static int applied;
    if (!warning_on_screen()) { applied = 0; return; }  /* re-arm: the game may reload it */
    warning_rule();                                     /* every frame: the list is rebuilt */
    if (applied) return;
    warning_replace();
    applied = 1;
    fprintf(stderr, "ddr: CAUTION screen replaced\n");
    fflush(stderr);
}

/* ── Memory-card messages in English ────────────────────────────────────────
 *
 * The page at tpage 0x0015 (VRAM 320,256) is a message board: band 0 is the
 * line that changes, bands 1 and 2 the fixed warning. Each band is drawn as
 * its own strip, cut to the width of the Japanese line it carries.
 *
 * Identification cannot be "three strips are on screen" -- that fired on every
 * message alike and painted "Now checking." over "now loading". Nor can it be
 * a hash of the whole band: the game overwrites only as many pixels as the new
 * line is wide, so the tail of the previous message survives and poisons the
 * hash. What IS stable is the pair the game itself provides: the width of the
 * strip, and the pixels under exactly that width. Unknown pair -> nothing is
 * touched, so an unseen message stays Japanese rather than turning into the
 * wrong sentence.
 */
#define MCARD_TPAGE  0x0015u
#define MCARD_CLUT   0x7C3Cu
#define MCARD_PAGE_X 320
#define MCARD_PAGE_Y 256

/* The palette holds several black entries, so "is this pixel the darkest
 * index" is not the same question as "is this pixel background" -- picking the
 * wrong black made every background pixel read as ink and no hash ever
 * matched. Compare the COLOUR the index resolves to instead: a texel is ink
 * when its palette colour is not black. */
static void mcard_palette(uint16_t *pal, int *ink, int *paper)
{
    int cx = (int)(MCARD_CLUT & 0x3Fu) * 16;
    int cy = (int)((MCARD_CLUT >> 6) & 0x1FFu);
    int best = -1, worst = 1 << 30;
    *ink = *paper = 0;
    for (int i = 0; i < 16; i++) {
        pal[i] = gpu_vram_peek(cx + i, cy);
        int lum = (int)((pal[i] & 0x1Fu) + ((pal[i] >> 5) & 0x1Fu) + ((pal[i] >> 10) & 0x1Fu));
        if (lum > best)  { best = lum;  *ink = i; }
        if (lum < worst) { worst = lum; *paper = i; }
    }
}

static uint32_t mcard_vram_hash(int band, int width, const uint16_t *pal)
{
    uint32_t h = 2166136261u;
    for (int y = band * 16; y < band * 16 + 16; y++)
        for (int x = 0; x < width; x++) {
            uint16_t hw  = gpu_vram_peek(MCARD_PAGE_X + x / 4, MCARD_PAGE_Y + y);
            int      idx = (int)((hw >> ((x & 3) * 4)) & 0x0Fu);
            h = (h ^ (uint32_t)((pal[idx] & 0x7FFFu) != 0u)) * 16777619u;
        }
    return h;
}

/* The same hash over one of our own bands, so a band we already replaced can be
 * recognised without keeping state the game could invalidate behind our back. */
static uint32_t mcard_bits_hash(int msg)
{
    const unsigned char *art = lang_is_pt() ? mcard_bits_pt : mcard_bits_en;
    const unsigned char *src = art + (size_t)msg * MCARD_BAND_BYTES;
    uint32_t h = 2166136261u;
    for (int y = 0; y < MCARD_BAND_H; y++) {
        const unsigned char *row = src + (size_t)y * (MCARD_BAND_W / 8);
        for (int x = 0; x < MCARD_BAND_W; x++)
            h = (h ^ (uint32_t)((row[x >> 3] >> (7 - (x & 7))) & 1)) * 16777619u;
    }
    return h;
}

static void mcard_upload_band(int band, int msg, int ink, int paper)
{
    gpu_write_gp0(0xA0000000u);
    gpu_write_gp0(((uint32_t)(MCARD_PAGE_Y + band * 16) << 16) | (uint32_t)MCARD_PAGE_X);
    gpu_write_gp0(((uint32_t)MCARD_BAND_H << 16) | 64u);

    const unsigned char *art = lang_is_pt() ? mcard_bits_pt : mcard_bits_en;
    const unsigned char *src = art + (size_t)msg * MCARD_BAND_BYTES;
    for (int y = 0; y < MCARD_BAND_H; y++) {
        const unsigned char *row = src + (size_t)y * (MCARD_BAND_W / 8);
        for (int hw = 0; hw < 64; hw += 2) {
            uint32_t word = 0u;
            for (int k = 0; k < 2; k++) {
                uint32_t half = 0u;
                for (int t = 0; t < 4; t++) {
                    int x   = (hw + k) * 4 + t;
                    int set = (row[x >> 3] >> (7 - (x & 7))) & 1;
                    half |= (uint32_t)(set ? ink : paper) << (t * 4);
                }
                word |= half << (k * 16);
            }
            gpu_write_gp0(word);
        }
    }
}

/* A textured quad is tag, C0, XY0, UV0|CLUT, C1, XY1, UV1|TPAGE, C2, XY2, UV2,
 * C3, XY3, UV3. A replaced line is re-cut to the full 256 at the long line's
 * left margin, in screen and texture space together, so the centred English is
 * not clipped to the width of the Japanese sentence it replaced. */
/* A textured quad is tag, C0, XY0, UV0|CLUT, C1, XY1, UV1|TPAGE, C2, XY2, UV2,
 * C3, XY3, UV3. A replaced line is re-cut to the full 256 at the long line's
 * left margin, in screen and texture space together, so the centred English is
 * not clipped to the width of the Japanese sentence it replaced. */
#define MCARD_LINE_X (-132)

static void mcard_widen(uint32_t p)
{
    psx_mod_write_half(p +  8u, (uint16_t)(int16_t)MCARD_LINE_X);         /* XY0.x */
    psx_mod_write_half(p + 32u, (uint16_t)(int16_t)MCARD_LINE_X);         /* XY2.x */
    psx_mod_write_half(p + 20u, (uint16_t)(int16_t)(MCARD_LINE_X + 255)); /* XY1.x */
    psx_mod_write_half(p + 44u, (uint16_t)(int16_t)(MCARD_LINE_X + 255)); /* XY3.x */
    psx_mod_write_byte(p + 12u, 0u);                                      /* UV0.u */
    psx_mod_write_byte(p + 36u, 0u);                                      /* UV2.u */
    psx_mod_write_byte(p + 24u, 255u);                                    /* UV1.u */
    psx_mod_write_byte(p + 48u, 255u);                                    /* UV3.u */
}

static void mcard_tick(void)
{
    uint32_t ctx = psx_mod_read_word(MENU_PADPTR);
    if (!ctx) return;
    uint32_t cur = psx_mod_read_word(ctx + PRIM_PTR_OFF);
    uint32_t s0  = psx_mod_read_word(PRIM_STARTS);
    uint32_t s1  = psx_mod_read_word(PRIM_STARTS + 4u);
    if (!in_ram(cur) || !in_ram(s0) || !in_ram(s1)) return;
    uint32_t p = (cur >= s1 && s1 > s0) ? s1 : ((cur >= s0) ? s0 : 0u);
    if (!p) return;

    uint16_t pal[16];
    int ink = 0, paper = 0;
    mcard_palette(pal, &ink, &paper);
    if (ink == paper) return;

    for (int guard = 0; p < cur && guard < 4096; guard++) {
        uint32_t len = psx_mod_read_byte(p + 3u);
        if (len == 0u) break;
        uint32_t end = p + (len + 1u) * 4u;
        uint32_t cmd = psx_mod_read_byte(p + 7u);
        if (len >= 3u && (cmd & 0x04u) && cmd >= 0x20u && cmd < 0x60u &&
            psx_mod_read_half(p + 14u) == MCARD_CLUT &&
            psx_mod_read_half(p + 26u) == MCARD_TPAGE) {

            int band  = psx_mod_read_byte(p + 13u) / 16;      /* v -> band */
            int x0    = (int16_t)psx_mod_read_half(p + 8u);
            int width = (int16_t)psx_mod_read_half(p + 20u) - x0;

            if (band >= 0 && band < 3 && width > 0 && width <= 256) {
                uint32_t h    = mcard_vram_hash(band, width, pal);
                int      ours = -1;

                {   /* PSX_MCHASH=1: what the runtime actually computes, so the
                     * table can be compared against it instead of guessed at. */
                    static int  show = -1;
                    static uint32_t seen[16];
                    static int  seen_n;
                    if (show < 0) { const char *e = getenv("PSX_MCHASH"); show = (e && *e && *e != '0'); }
                    if (show) {
                        int dup = 0;
                        for (int k = 0; k < seen_n; k++) if (seen[k] == h) dup = 1;
                        if (!dup && seen_n < 16) {
                            seen[seen_n++] = h;
                            fprintf(stderr, "ddr: banda %d  largura %3d  hash 0x%08X"
                                            "  (tinta=%d papel=%d)\n",
                                    band, width, h, ink, paper);
                            fflush(stderr);
                        }
                    }
                }

                for (int i = 0; i < MCARD_MSG_COUNT; i++)
                    if (mcard_msgs[i].width == width && mcard_msgs[i].hash == h) {
                        mcard_upload_band(band, mcard_msgs[i].band, ink, paper);
                        ours = i;
                        fprintf(stderr, "ddr: memory-card line %d replaced (w=%d)\n",
                                band, width);
                        fflush(stderr);
                        break;
                    }

                /* Already replaced on an earlier frame? Then the band matches
                 * one of our own pictures across the full 256. */
                if (ours < 0) {
                    uint32_t full = mcard_vram_hash(band, MCARD_BAND_W, pal);
                    for (int i = 0; i < MCARD_MSG_COUNT; i++)
                        if (full == mcard_bits_hash(mcard_msgs[i].band)) { ours = i; break; }
                }

                /* Widen ONLY what we own, and only where the entry asks for it:
                 * a scaled strip keeps the geometry the game gave it. */
                if (ours >= 0 && mcard_msgs[ours].widen) mcard_widen(p);
                (void)x0;
            }
        }
        p = end;
    }
}

/* ── Message-page watch (PSX_MCWATCH=1) ─────────────────────────────────────
 *
 * The page at VRAM 320,256 is the game's message board: the memory-card check
 * lives there, and so do the save/load notices, each overwriting the last. A
 * strip-layout trigger only sees the ones that happen to be on screen when the
 * probe runs; watching the PIXELS catches every message that passes through,
 * however briefly, because the page is rewritten for each one.
 *
 * Hashes the top 48 rows every frame and writes a PPM whenever the hash moves,
 * logging the strips being drawn at that moment so the layout of each message
 * is recorded next to its picture.
 */
#define MC_PAGE_X 320
#define MC_PAGE_Y 256
#define MC_ROWS   64

static void mc_watch(void)
{
    static int      enabled = -1;
    static uint32_t last_hash;
    static int      seq;

    if (enabled < 0) { const char *e = getenv("PSX_MCWATCH"); enabled = (e && *e && *e != '0'); }
    if (!enabled || seq >= 24) return;

    uint32_t h = 2166136261u;
    for (int y = 0; y < MC_ROWS; y++)
        for (int x = 0; x < 64; x++)
            h = (h ^ gpu_vram_peek(MC_PAGE_X + x, MC_PAGE_Y + y)) * 16777619u;
    if (h == last_hash) return;
    last_hash = h;

    int cx = (int)(MCARD_CLUT & 0x3Fu) * 16;
    int cy = (int)((MCARD_CLUT >> 6) & 0x1FFu);
    char path[64];
    snprintf(path, sizeof path, "mc_%02d.ppm", seq);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n256 %d\n255\n", MC_ROWS);
        for (int y = 0; y < MC_ROWS; y++) {
            for (int u = 0; u < 256; u++) {
                uint16_t hw  = gpu_vram_peek(MC_PAGE_X + u / 4, MC_PAGE_Y + y);
                uint8_t  idx = (uint8_t)((hw >> ((u & 3) * 4)) & 0x0Fu);
                uint16_t c   = gpu_vram_peek(cx + idx, cy);
                unsigned char rgb[3];
                rgb[0] = (unsigned char)(((c      ) & 0x1Fu) << 3);
                rgb[1] = (unsigned char)(((c >>  5) & 0x1Fu) << 3);
                rgb[2] = (unsigned char)(((c >> 10) & 0x1Fu) << 3);
                fwrite(rgb, 1, 3, f);
            }
        }
        fclose(f);
    }

    fprintf(stderr, "ddr: mc_%02d.ppm gravado\n", seq);
    fflush(stderr);
    seq++;
}

/* ── Counter hunt (PSX_HUNT=1) ──────────────────────────────────────────────
 *
 * Finds the judgement counters the way a value scanner does, because reading
 * the code blind is slower: a counter that tallies PERFECTs goes up exactly
 * when you hit an arrow, while a frame timer goes up EVERY frame. Counting, per
 * word, how many frames it incremented by one separates the two -- a tally
 * moves on a few percent of frames, a clock on all of them.
 *
 * Scans the two regions the game keeps state in (the save block around
 * 0x80010000 and the working area from 0x80070000), which keeps the per-frame
 * cost to ~96K reads instead of half a million.
 */
#define HUNT_R0_LO 0x80010000u
#define HUNT_R0_HI 0x80020000u
#define HUNT_R1_LO 0x80070000u
#define HUNT_R1_HI 0x800C0000u
#define HUNT_WORDS (((HUNT_R0_HI - HUNT_R0_LO) + (HUNT_R1_HI - HUNT_R1_LO)) / 4u)

static uint32_t hunt_addr_of(uint32_t i)
{
    uint32_t n0 = (HUNT_R0_HI - HUNT_R0_LO) / 4u;
    return (i < n0) ? (HUNT_R0_LO + i * 4u) : (HUNT_R1_LO + (i - n0) * 4u);
}

static void hunt_tick(void)
{
    static int       enabled = -1;
    static uint32_t *shadow;
    static uint16_t *bumps;      /* frames in which this word went up by one */
    static uint32_t  frames;

    if (enabled < 0) { const char *e = getenv("PSX_HUNT"); enabled = (e && *e && *e != '0'); }
    if (!enabled) return;

    if (!shadow) {
        shadow = (uint32_t *)calloc(HUNT_WORDS, sizeof *shadow);
        bumps  = (uint16_t *)calloc(HUNT_WORDS, sizeof *bumps);
        if (!shadow || !bumps) { enabled = 0; return; }
        for (uint32_t i = 0; i < HUNT_WORDS; i++) shadow[i] = psx_mod_read_word(hunt_addr_of(i));
        fprintf(stderr, "ddr: hunt armado (%u palavras)\n", (unsigned)HUNT_WORDS);
        fflush(stderr);
        return;
    }

    frames++;
    for (uint32_t i = 0; i < HUNT_WORDS; i++) {
        uint32_t v = psx_mod_read_word(hunt_addr_of(i));
        if (v == shadow[i] + 1u && bumps[i] < 0xFFFFu) bumps[i]++;
        shadow[i] = v;
    }

    if (frames % 1800u) return;        /* a report every ~30 s at 60 Hz */

    fprintf(stderr, "ddr: --- hunt: %u quadros ---\n", (unsigned)frames);
    for (uint32_t i = 0; i < HUNT_WORDS; i++) {
        /* A tally moves often enough to notice and far less than every frame;
         * anything above 60%% of frames is a clock, anything below a handful is
         * noise. */
        uint32_t b = bumps[i];
        if (b < 8u || b * 100u > frames * 60u) continue;
        fprintf(stderr, "ddr:   %08X  +1 em %u de %u quadros  valor=%u\n",
                hunt_addr_of(i), (unsigned)b, (unsigned)frames,
                (unsigned)psx_mod_read_word(hunt_addr_of(i)));
    }
    fflush(stderr);
}

/* ── Pointer finder / memory peek (PSX_PTRFIND, PSX_PEEK) ───────────────────
 *
 * Some variables have no static reference at all: the code reaches them
 * through a pointer held in RAM, so no lui/addiu pair in the image names them
 * and a disassembly search comes up empty. Finding the POINTER gives the thread
 * back -- the pointer variable itself is usually statically addressed, and from
 * there the disassembler can follow the code that uses it.
 *
 *   PSX_PTRFIND=0x800921D4   report every word in RAM holding a value within a
 *                            kilobyte below that address (i.e. a pointer into
 *                            the same structure), once.
 *   PSX_PEEK=0x80092180:64   dump 64 words from that address every ~5 s.
 */
static void ptr_peek_tick(void)
{
    static int      init;
    static uint32_t find_target, peek_addr, peek_words, frame;

    if (!init) {
        init = 1;
        const char *f = getenv("PSX_PTRFIND");
        if (f && *f) find_target = (uint32_t)strtoul(f, NULL, 0);
        const char *k = getenv("PSX_PEEK");
        if (k && *k) {
            peek_addr = (uint32_t)strtoul(k, NULL, 0);
            const char *c = strchr(k, ':');
            peek_words = c ? (uint32_t)strtoul(c + 1, NULL, 0) : 16u;
            if (peek_words > 256u) peek_words = 256u;
        }
    }

    /* The scan runs DURING play, not at boot: at boot the structure does not
     * exist yet and the pointer has not been stored, which is why an early scan
     * comes back empty. Twice, so a pointer that only appears mid-song is still
     * caught. */
    if (find_target) {
        static int done;
        frame++;
        if ((frame == 900u || frame == 2700u) && done < 2) {
            done++;
            fprintf(stderr, "ddr: --- ponteiros para perto de %08X (quadro %u) ---\n",
                    (unsigned)find_target, (unsigned)frame);
            for (uint32_t a = 0x80010000u; a < 0x801F0000u; a += 4u) {
                uint32_t v = psx_mod_read_word(a);
                uint32_t d = (v <= find_target) ? (find_target - v) : (v - find_target);
                if (d <= 0x400u)
                    fprintf(stderr, "ddr:   %08X -> %08X  (alvo em %s%u)\n",
                            (unsigned)a, (unsigned)v,
                            (v <= find_target) ? "+" : "-", (unsigned)d);
            }
            fflush(stderr);
        }
    }

    if (!peek_addr) return;
    if (frame++ % 300u) return;
    fprintf(stderr, "ddr: peek %08X:\n", (unsigned)peek_addr);
    for (uint32_t i = 0; i < peek_words; i += 4u) {
        fprintf(stderr, "ddr:   %08X ", (unsigned)(peek_addr + i * 4u));
        for (uint32_t k = 0; k < 4u && i + k < peek_words; k++)
            fprintf(stderr, " %08X", (unsigned)psx_mod_read_word(peek_addr + (i + k) * 4u));
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

/* ── Judgement window ───────────────────────────────────────────────────────
 *
 * The game grades a step with three range checks on the timing error, in plain
 * arithmetic, at 0x8005C724..0x8005C758:
 *
 *     addiu v0,s0,2 ; sltiu v0,v0,5    delta in [-2,+2]  -> 1 PERFECT
 *     addiu v0,s0,5 ; sltiu v0,v0,12   delta in [-5,+6]  -> 2 GREAT
 *     addiu v0,s0,9 ; sltiu v0,v0,19   delta in [-9,+9]  -> 3 GOOD
 *                                      otherwise         -> 4 BOO
 *
 * The unsigned compare is the idiom: delta + K < W means -K <= delta <= W-1-K.
 * (GREAT is asymmetric -- one unit more on the positive side. That is the
 * game's own choice, and scaling preserves it.)
 *
 * s0 counts FRAMES, which is why this lives next to the frame-rate mod: at
 * 60 FPS the same real-world error counts twice as many units, so every window
 * is half as long in milliseconds -- PERFECT drops from about +/-66 ms to about
 * +/-33 ms. Nothing is broken; the ruler got finer. So the 60 FPS mod scales
 * ALL THREE windows by two, which keeps the grading exactly as faithful as it
 * was at 30 FPS. Scaling only PERFECT would have been worse than scaling none:
 * a step that used to be GREAT would start reading GOOD.
 *
 * The Judgement Window mod then overrides PERFECT explicitly, and its value
 * wins over the automatic compensation -- a player who asks for a number gets
 * that number. GREAT and GOOD keep the faithful scaling underneath.
 *
 * Each immediate is replaced through psx_mod_write_code_word, which routes the
 * address through the executable-RAM path so the recompiled block cannot keep
 * serving the old constant. The SHAPE of every word is verified before writing,
 * so a wrong address scribbles nothing. Feeding the stock numbers back through
 * the same formula reproduces the original words bit for bit, which is how the
 * arithmetic was checked before it ever ran.
 */
#define JUDGE_ADDIU_SHAPE 0x26020000u   /* addiu v0, s0, imm */
#define JUDGE_SLTIU_SHAPE 0x2C420000u   /* sltiu v0, v0, imm */

typedef struct { uint32_t addiu_addr, sltiu_addr; int k, w; } JudgeBand;

/* Stock windows, in the order the game tests them. */
static const JudgeBand JUDGE_BANDS[3] = {
    { 0x8005C724u, 0x8005C728u, 2,  5 },   /* PERFECT: [-2,+2] */
    { 0x8005C73Cu, 0x8005C740u, 5, 12 },   /* GREAT:   [-5,+6] */
    { 0x8005C754u, 0x8005C758u, 9, 19 },   /* GOOD:    [-9,+9] */
};

static void judge_write_band(const JudgeBand *b, int k, int w)
{
    uint32_t want_a = JUDGE_ADDIU_SHAPE | (uint32_t)(k & 0xFFFF);
    uint32_t want_b = JUDGE_SLTIU_SHAPE | (uint32_t)(w & 0xFFFF);
    uint32_t cur_a  = psx_mod_read_word(b->addiu_addr);
    uint32_t cur_b  = psx_mod_read_word(b->sltiu_addr);

    if ((cur_a & 0xFFFF0000u) != JUDGE_ADDIU_SHAPE ||
        (cur_b & 0xFFFF0000u) != JUDGE_SLTIU_SHAPE) return;   /* not the code we mapped */
    if (cur_a == want_a && cur_b == want_b) return;

    psx_mod_write_code_word(b->addiu_addr, want_a);
    psx_mod_write_code_word(b->sltiu_addr, want_b);
    fprintf(stderr, "ddr: janela %08X: %d..%+d quadros\n",
            (unsigned)b->addiu_addr, -k, w - 1 - k);
    fflush(stderr);
}

static void judge_tick(void)
{
    static int touched;

    /* 60 FPS halves every window in real time, so it doubles every window in
     * frames. Off, the scale is 1 and the stock numbers are restored. */
    int scale = s_feat_fps60 ? 2 : 1;
    if (!s_feat_judge && !s_feat_fps60 && !touched) return;
    touched = 1;

    for (int i = 0; i < 3; i++) {
        int k = JUDGE_BANDS[i].k * scale;
        int w = (JUDGE_BANDS[i].w - 1) * scale + 1;

        if (i == 0 && s_feat_judge) {        /* the player's own PERFECT wins */
            int n = option_int_pkg("ddr.judge", "window", "perfect", 2 * scale);
            int c = option_int_pkg("ddr.judge", "window", "shift", 0);
            if (n < 1) n = 1;
            if (n > 16) n = 16;
            if (c < -8) c = -8;
            if (c > 8) c = 8;
            k = n - c;
            w = 2 * n + 1;
        }
        judge_write_band(&JUDGE_BANDS[i], k, w);
    }
}

/* ── Whole-VRAM snapshot (PSX_VRAMFULL=<frames>) ───────────────────────────
 *
 * A map, not a rip: 1024x512 halfwords straight out of the frame store, read as
 * 15-bit colour. Anything stored as 16bpp art shows up as itself; 4bpp and 8bpp
 * blocks show up as noise, but their POSITION and SIZE are plainly visible,
 * which is what a search needs. Extract those properly afterwards with
 * PSX_VRAMDUMP, which decodes with a real palette.
 *
 * Deriving a picture's address from the texture page a draw call names is the
 * alternative, and it is not reliable for rectangles: a rectangle has no page
 * of its own, and the draw-mode command that sets one precedes it in the
 * ORDERING TABLE, not in the primitive buffer this probe walks. Reading VRAM
 * by coordinates sidesteps the question entirely.
 */
static void vram_full_tick(void)
{
    static int      init, seq;
    static uint32_t every, frame;

    if (!init) {
        init = 1;
        const char *e = getenv("PSX_VRAMFULL");
        every = (e && *e) ? (uint32_t)strtoul(e, NULL, 0) : 0u;
    }
    /* Periodic, not a single instant: a screen that lasts twenty seconds before
     * timing out cannot be caught by a frame number chosen in advance. Snapshots
     * are numbered, so a session can be walked through and sorted out later. */
    if (!every || seq >= 12) return;
    if (++frame % every) return;

    char path[32];
    snprintf(path, sizeof path, "vram_full_%02d.ppm", seq);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n1024 512\n255\n");
    for (int y = 0; y < 512; y++)
        for (int x = 0; x < 1024; x++) {
            uint16_t c = gpu_vram_peek(x, y);
            unsigned char rgb[3];
            rgb[0] = (unsigned char)(((c      ) & 0x1Fu) << 3);
            rgb[1] = (unsigned char)(((c >>  5) & 0x1Fu) << 3);
            rgb[2] = (unsigned char)(((c >> 10) & 0x1Fu) << 3);
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    fprintf(stderr, "ddr: %s gravado no quadro %u\n", path, (unsigned)frame);
    fflush(stderr);
    seq++;
}

/* ── VRAM region dump (PSX_VRAMDUMP="x,y,w,h,clut[,x2,y2,w2]") ──────────────
 *
 * The texture probe keys on the page a draw call names, which is enough when a
 * picture sits in one page. The menu's message band does not: it is 320 texels
 * wide, a 4bpp page holds 256, so the game splits it across two pages -- and
 * the larger half is uploaded OVER the font sheet, so a dump taken at boot
 * shows the font instead of the message.
 *
 * This reads a rectangle straight out of VRAM by coordinates, decoded with a
 * palette given by hand, and can stitch a second rectangle onto its right edge
 * so a split image comes out whole.
 */
static void vram_dump_tick(void)
{
    static int init, done;
    static int x0, y0, w0, h0, x1, y1, w1;
    static uint32_t clut;
    static uint32_t frame;

    if (!init) {
        init = 1;
        const char *e = getenv("PSX_VRAMDUMP");
        if (!e || !*e) { done = 1; return; }
        unsigned a=0,b=0,c=0,d=0,cl=0,a2=0,b2=0,c2=0;
        int n = sscanf(e, "%u,%u,%u,%u,%x,%u,%u,%u", &a,&b,&c,&d,&cl,&a2,&b2,&c2);
        if (n < 5) { done = 1; return; }
        x0=(int)a; y0=(int)b; w0=(int)c; h0=(int)d; clut=cl;
        if (n >= 8) { x1=(int)a2; y1=(int)b2; w1=(int)c2; }
    }
    /* Wait for the picture to be ON SCREEN rather than for a number of frames.
     * The page is reused between screens and the menu returns to the attract
     * loop after a few idle seconds, so any fixed delay is a guess. A primitive
     * naming the requested palette IS the picture being drawn -- that is the
     * moment to read VRAM. */
    if (done) return;
    frame++;
    {
        uint32_t ctx = psx_mod_read_word(MENU_PADPTR);
        if (!ctx) return;
        uint32_t cur = psx_mod_read_word(ctx + PRIM_PTR_OFF);
        uint32_t s0a = psx_mod_read_word(PRIM_STARTS);
        uint32_t s1a = psx_mod_read_word(PRIM_STARTS + 4u);
        if (!in_ram(cur) || !in_ram(s0a) || !in_ram(s1a)) return;
        uint32_t q = (cur >= s1a && s1a > s0a) ? s1a : ((cur >= s0a) ? s0a : 0u);
        if (!q) return;
        int seen = 0;
        for (int guard = 0; q < cur && guard < 4096; guard++) {
            uint32_t len = psx_mod_read_byte(q + 3u);
            if (len == 0u) break;
            uint32_t cmd = psx_mod_read_byte(q + 7u);
            if (len >= 3u && (cmd & 0x04u) && psx_mod_read_half(q + 14u) == (uint16_t)clut) {
                seen = 1; break;
            }
            q += (len + 1u) * 4u;
        }
        if (!seen) return;
    }
    done = 1;

    int cx = (int)(clut & 0x3Fu) * 16;
    int cy = (int)((clut >> 6) & 0x1FFu);
    int w  = w0 + w1;

    char path[64];
    snprintf(path, sizeof path, "vram_%d_%d.ppm", x0, y0);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h0);
    for (int y = 0; y < h0; y++) {
        for (int u = 0; u < w; u++) {
            int sx = (u < w0) ? (x0 + u / 4) : (x1 + (u - w0) / 4);
            int sy = (u < w0) ? (y0 + y)     : (y1 + y);
            int nib = (u < w0) ? (u & 3)     : ((u - w0) & 3);
            uint16_t hw  = gpu_vram_peek(sx, sy);
            uint8_t  idx = (uint8_t)((hw >> (nib * 4)) & 0x0Fu);
            uint16_t c   = gpu_vram_peek(cx + idx, cy);
            unsigned char rgb[3];
            rgb[0] = (unsigned char)(((c      ) & 0x1Fu) << 3);
            rgb[1] = (unsigned char)(((c >>  5) & 0x1Fu) << 3);
            rgb[2] = (unsigned char)(((c >> 10) & 0x1Fu) << 3);
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    fprintf(stderr, "ddr: %s gravado no quadro %u (%dx%d, paleta em %d,%d)\n",
            path, (unsigned)frame, w, h0, cx, cy);
    fflush(stderr);
}

/* ── Main-menu safety band ──────────────────────────────────────────────────
 *
 * A 320x40 picture at VRAM (320,256), 4bpp, palette (512,244): dark green
 * ground, white text. Being 320 wide it crosses a texture-page boundary, which
 * is why the game draws it as two rectangles -- but in VRAM it is contiguous,
 * so one transfer replaces it.
 *
 * The same page carries the memory-card messages, so the trigger is the
 * picture itself: the region is hashed (a texel counts as ink when it differs
 * from the top-left one, which is background by construction) and the
 * replacement fires only on the Japanese band's exact hash. Anything else --
 * another message, our own English already in place -- is left alone.
 */
static uint32_t band_hash(int bg_idx, const uint16_t *pal)
{
    uint32_t h = 2166136261u;
    for (int y = 0; y < BAND_H; y++)
        for (int x = 0; x < BAND_W; x++) {
            uint16_t hw  = gpu_vram_peek(BAND_X + x / 4, BAND_Y + y);
            int      idx = (int)((hw >> ((x & 3) * 4)) & 0x0Fu);
            int      ink = (pal[idx] & 0x7FFFu) != (pal[bg_idx] & 0x7FFFu);
            h = (h ^ (uint32_t)ink) * 16777619u;
        }
    return h;
}

static void band_tick(void)
{
    if (!s_feat_warning) return;

    int cx = (int)(BAND_CLUT & 0x3Fu) * 16;
    int cy = (int)((BAND_CLUT >> 6) & 0x1FFu);
    uint16_t pal[16];
    int ink = 0, best = -1;
    for (int i = 0; i < 16; i++) {
        pal[i] = gpu_vram_peek(cx + i, cy);
        int lum = (int)((pal[i] & 0x1Fu) + ((pal[i] >> 5) & 0x1Fu) + ((pal[i] >> 10) & 0x1Fu));
        if (lum > best) { best = lum; ink = i; }
    }
    /* A corner of the band is background, so its index is the ground. */
    int bg = (int)(gpu_vram_peek(BAND_X, BAND_Y) & 0x0Fu);
    if (ink == bg) return;
    if (band_hash(bg, pal) != BAND_JP_HASH) return;

    const unsigned char *art = lang_is_pt() ? band_bits_pt : band_bits_en;

    gpu_write_gp0(0xA0000000u);
    gpu_write_gp0(((uint32_t)BAND_Y << 16) | (uint32_t)BAND_X);
    gpu_write_gp0(((uint32_t)BAND_H << 16) | (uint32_t)(BAND_W / 4));   /* halfwords */

    for (int y = 0; y < BAND_H; y++) {
        const unsigned char *row = art + (size_t)y * (BAND_W / 8);
        for (int hw = 0; hw < BAND_W / 4; hw += 2) {
            uint32_t word = 0u;
            for (int k = 0; k < 2; k++) {
                uint32_t half = 0u;
                for (int t = 0; t < 4; t++) {
                    int x   = (hw + k) * 4 + t;
                    int set = (row[x >> 3] >> (7 - (x & 7))) & 1;
                    half |= (uint32_t)(set ? ink : bg) << (t * 4);
                }
                word |= half << (k * 16);
            }
            gpu_write_gp0(word);
        }
    }
    fprintf(stderr, "ddr: menu safety band replaced\n");
    fflush(stderr);
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
    hunt_tick();
    ptr_peek_tick();
    vram_dump_tick();
    vram_full_tick();
    vsync_force();
    prim_dump();
    tim_scan();
    tex_dump();
    mc_watch();
    judge_tick();
    if (s_feat_warning) { warning_tick(); mcard_tick(); band_tick(); }

    if (s_feat_bga_dark) {
        bga_flush();                 /* the last call of the frame */
        s_bga_mark = 0;
        s_bga_pct  = option_int_pkg("ddr.bga", "dark", "brightness", 50);
    }

    if (s_feat_dim_bg)
        dim_background(option_int_pkg("ddr.background", "dim", "brightness", 50));

    /* Last: it rewrites a packet's length, so the linear walkers above must
     * have finished with the buffer before this runs. */
    if (s_feat_menu_bd)
        menu_backdrop(option_int_pkg("ddr.menu", "backdrop", "level", 100));

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
    (void)psx_mod_register_activation_plugin("ddr.widescreen.wide", ddr_activate_widescreen);
    (void)psx_mod_register_activation_plugin("ddr.menu.backdrop",   ddr_activate_menu_bd);
    (void)psx_mod_register_activation_plugin("ddr.warning.english", ddr_activate_warning);
    (void)psx_mod_register_activation_plugin("ddr.timing.offset",   ddr_activate_timing);
    (void)psx_mod_register_activation_plugin("ddr.judge.window",    ddr_activate_judge);

    (void)psx_mod_register_function_entry_plugin("ddr.menu",  DDR_MENU_SCREEN, on_menu_screen);
    (void)psx_mod_register_function_entry_plugin("ddr.menu",  DDR_MENU_DRAW,   on_menu_draw);
    (void)psx_mod_register_function_entry_plugin("ddr.dedup", DDR_MODEL_EMIT,  on_model_emit);
    (void)psx_mod_register_function_entry_plugin("ddr.dedup", DDR_OT_MERGE,    on_ot_merge);
    (void)psx_mod_register_function_entry_plugin("ddr.anim",  DDR_ANIM_DRIVER, on_anim_driver);
}
