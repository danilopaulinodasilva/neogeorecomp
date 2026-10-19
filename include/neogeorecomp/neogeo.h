/*
 * neogeo.h — what a game host sees of the runtime.
 *
 * A game repo supplies an ng_game_t (its ROM layout) and the table of
 * recompiled routines that tools/m68krecomp generated from the user's own
 * dump, then hands both to ng_main(), which owns the command line, ROM
 * loading, the frame loop, video/audio output and input.
 */
#ifndef NEOGEORECOMP_NEOGEO_H
#define NEOGEORECOMP_NEOGEO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ng_func_t)(void);

/* A copy of sprite `slot` at hardware X `x` (0-511), mirrored when `flip`
 * is set. It is drawn right after the original, so it keeps its place in
 * the sprite order. */
typedef struct {
    uint16_t slot;
    uint16_t x;
    uint8_t  flip;
} ng_extra_sprite_t;

/* One recompiled entry point: the 68k address control can arrive at, and
 * the routine that continues from there. */
typedef struct {
    uint32_t  addr;
    ng_func_t fn;
} ng_func_entry_t;

/* A slice of a ROM file copied into a region. `file` is looked up in the
 * ROM directory. `swap` byte-swaps each 16-bit word (MAME stores P ROMs and
 * the system ROM little-endian). */
typedef struct {
    const char *file;
    uint32_t    file_offset;
    uint32_t    dest_offset;
    uint32_t    length;
    int         swap;
} ng_rom_part_t;

#define NG_MAX_PARTS 8

typedef struct {
    const char   *name;               /* short name, e.g. "mslug" */
    const char   *title;              /* window title */
    uint32_t      prom_size;          /* fixed 1 MB + banked area */
    ng_rom_part_t prom[NG_MAX_PARTS];
    uint32_t      crom_size;          /* bytes after pairing */
    const char   *crom_pairs[NG_MAX_PARTS][2];  /* odd/even bitplane files */
    const char   *srom;
    const char   *m1;
    const char   *vrom[NG_MAX_PARTS];
    const char   *bios;               /* default system ROM; NULL = sp-s2.sp1 (MVS) */
    int           aes;                /* 1 = run as an AES console (cart needs it) */
    /* Optional: changes the loaded P ROM (68k byte order) before the board
     * starts, given the output width (--width). A game uses it for options
     * such as widescreen, which only make sense for that game. */
    void        (*patch)(uint8_t *prom, size_t size, int screen_w);
    /* The P ROM words `patch` may change, ending with 0 (banked addresses
     * in the recompiler's form, bank in bits 24-31). Recompiled code that
     * uses one as an immediate operand reads it from memory instead of
     * baking it in, so it sees the patched value. */
    const uint32_t *live_words;
    /* Optional, for output wider than 320: called before each frame is
     * drawn, with sprite VRAM (SCB1 at 0, SCB2-4 at $8000-$8400). It writes
     * up to `max` sprite copies to `out` and returns how many, so a game
     * can fill the margins of screens drawn for 320 pixels from its own
     * graphics. Copies change only the picture, never VRAM. */
    int (*extend)(const uint16_t *vram, int screen_w, ng_extra_sprite_t *out, int max);
} ng_game_t;

/* Controller state for one frame. Bits are active-high here; io.c inverts. */
typedef struct {
    uint8_t p1, p2;         /* bit 0 up, 1 down, 2 left, 3 right, 4 A, 5 B, 6 C, 7 D */
    uint8_t start1, start2, select1, select2;
    uint8_t coin1, coin2, service, test;
} ng_input_t;

enum {
    NG_UP = 0x01, NG_DOWN = 0x02, NG_LEFT = 0x04, NG_RIGHT = 0x08,
    NG_A = 0x10, NG_B = 0x20, NG_C = 0x40, NG_D = 0x80
};

int ng_main(int argc, char **argv, const ng_game_t *game,
            const ng_func_entry_t *funcs, size_t nfuncs);

#ifdef __cplusplus
}
#endif
#endif
