/*
 * neogeo.c — ng_main(): command line, ROM loading, and the frame loop.
 *
 * A frame is 264 lines of 768 68k clocks. The loop runs the CPU to the
 * start of each line, renders visible lines as they are reached, raises
 * VBlank (IRQ1) at line 240 pixel $11F, and services the LSPC timer
 * whenever its deadline falls inside the slice.
 *
 * Input comes from the window, or from an --input script so headless runs
 * are reproducible: `<frame>[-<end>] <button>[+<button>...]`, one per line.
 * A word that is not a button is a debug command (`save`, `load`, or one
 * of the game's ng_debug_item_t tokens) and runs once, on that frame.
 */
#include "ng_internal.h"
#include <neogeorecomp/bus.h>
#include <neogeorecomp/cpu.h>
#include "romload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t ng_frame_start;

static uint32_t s_fb[NG_MAX_SCREEN_W * NG_SCREEN_H];
static const ng_game_t *s_game;
static char s_status[48];

/* ---- options ---- */

#define MAX_SHOTS 64
typedef struct {
    const char *rom_path, *bios_path, *record, *input, *dump_misses, *bios_name, *wav;
    int headless, interp, verify, scale, mute, width_set;
    long frames;
    struct { long frame; const char *path; } shots[MAX_SHOTS];
    int nshots;
} options_t;

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [options]\n"
        "  --rom-path DIR        game ROM files (default: roms)\n"
        "  --bios-path DIR       system ROM files (default: the ROM path)\n"
        "  --bios FILE           system ROM (default: the game's, else sp-s2.sp1, MVS Asia/Europe)\n"
        "  --headless            no window; run as fast as possible\n"
        "  --record FILE.mp4     pipe frames to ffmpeg\n"
        "  --frames N            stop after N frames\n"
        "  --screenshot N:FILE   save frame N as PNG (repeatable)\n"
        "  --input FILE          scripted input (see docs/running.md)\n"
        "  --interp              run everything in the interpreter\n"
        "  --verify              re-run every recompiled block in the interpreter and compare\n"
        "  --dump-misses FILE    write interpreted entry points for the recompiler\n"
        "  --scale N             window scale (default 3)\n"
        "  --width N             output width, 320-%d: margins show what lies just off screen\n"
        "  --widescreen          --width 400 (16:9 with square pixels)\n"
        "  --wav FILE            write the audio to a WAV file\n"
        "  --mute                no sound from the window\n", prog, NG_MAX_SCREEN_W);
}

static int parse_args(int argc, char **argv, options_t *o) {
    memset(o, 0, sizeof(*o));
    o->rom_path = "roms"; o->scale = 3; o->frames = -1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--rom-path") && next) { o->rom_path = next; i++; }
        else if (!strcmp(a, "--bios-path") && next) { o->bios_path = next; i++; }
        else if (!strcmp(a, "--bios") && next) { o->bios_name = next; i++; }
        else if (!strcmp(a, "--headless")) o->headless = 1;
        else if (!strcmp(a, "--record") && next) { o->record = next; i++; }
        else if (!strcmp(a, "--frames") && next) { o->frames = atol(next); i++; }
        else if (!strcmp(a, "--input") && next) { o->input = next; i++; }
        else if (!strcmp(a, "--interp")) o->interp = 1;
        else if (!strcmp(a, "--verify")) o->verify = 1;
        else if (!strcmp(a, "--dump-misses") && next) { o->dump_misses = next; i++; }
        else if (!strcmp(a, "--scale") && next) { o->scale = atoi(next); i++; }
        else if (!strcmp(a, "--width") && next) { ng_screen_w = atoi(next); o->width_set = 1; i++; }
        else if (!strcmp(a, "--widescreen")) { ng_screen_w = 400; o->width_set = 1; }
        else if (!strcmp(a, "--wav") && next) { o->wav = next; i++; }
        else if (!strcmp(a, "--mute")) o->mute = 1;
        else if (!strcmp(a, "--screenshot") && next) {
            const char *colon = strchr(next, ':');
            if (!colon || o->nshots == MAX_SHOTS) { usage(argv[0]); return -1; }
            o->shots[o->nshots].frame = atol(next);
            o->shots[o->nshots].path = colon + 1;
            o->nshots++; i++;
        }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); exit(0); }
        else { usage(argv[0]); return -1; }
    }
    if (!o->bios_path) o->bios_path = o->rom_path;
    /* even, so both margins are equal */
    if (ng_screen_w < NG_SCREEN_W || ng_screen_w > NG_MAX_SCREEN_W || (ng_screen_w & 1)) {
        fprintf(stderr, "error: --width must be an even number from %d to %d\n", NG_SCREEN_W, NG_MAX_SCREEN_W);
        return -1;
    }
    return 0;
}

/* ---- scripted input ---- */

typedef struct { long start, end; ng_input_t in; } script_ev_t;
static script_ev_t *s_script;
static int s_nscript;

typedef struct { long frame; char cmd[64]; } dbg_ev_t;
static dbg_ev_t *s_dbg;
static int s_ndbg;

static const char *slot_path(void) {
    static char path[64];
    snprintf(path, sizeof(path), "%s.slot", s_game && s_game->name ? s_game->name : "neogeo");
    return path;
}

static int debug_nitems(void) {
    int n = 0;
    if (s_game && s_game->debug)
        for (const ng_debug_item_t *d = s_game->debug; d->token; d++) n++;
    return n;
}

static const ng_debug_item_t *debug_item(int index) {
    if (!s_game || !s_game->debug || index < 0) return NULL;
    const ng_debug_item_t *d = s_game->debug;
    for (int i = 0; d->token && i < index; i++) d++;
    return d->token ? d : NULL;
}

static void debug_apply(const ng_debug_item_t *d, int has, int value) {
    if (!d->set) return;
    if (d->max < d->min) { d->set(0); return; }
    if (!has) value = (d->min == 0 && d->max == 1) ? 1 : (d->get ? d->get() : d->min);
    if (value < d->min) value = d->min;
    if (value > d->max) value = d->max;
    d->set(value);
}

static void debug_run(const char *cmd) {
    char name[64];
    const ng_debug_item_t *d;
    char *eq;
    if (!strcmp(cmd, "save")) { ng_state_save(); return; }
    if (!strcmp(cmd, "load")) { ng_state_load(); return; }
    snprintf(name, sizeof(name), "%s", cmd);
    eq = strchr(name, '=');
    if (eq) *eq = 0;
    if (!s_game || !s_game->debug) return;
    for (d = s_game->debug; d->token; d++)
        if (!strcmp(d->token, name)) { debug_apply(d, eq != NULL, eq ? atoi(eq + 1) : 0); return; }
}

/* A script word the Debug page understands. Stored for the frame it names;
 * a range still fires once, at its first frame. */
static int debug_accept(long frame, const char *tok) {
    char name[64];
    char *eq;
    dbg_ev_t *grown;
    const ng_debug_item_t *d;
    if (strcmp(tok, "save") && strcmp(tok, "load")) {
        if (!s_game || !s_game->debug) return 0;
        snprintf(name, sizeof(name), "%s", tok);
        eq = strchr(name, '=');
        if (eq) *eq = 0;
        for (d = s_game->debug; d->token; d++)
            if (!strcmp(d->token, name)) break;
        if (!d->token) return 0;
    }
    grown = (dbg_ev_t *)realloc(s_dbg, (size_t)(s_ndbg + 1) * sizeof(*s_dbg));
    if (!grown) return 0;
    s_dbg = grown;
    s_dbg[s_ndbg].frame = frame;
    snprintf(s_dbg[s_ndbg].cmd, sizeof(s_dbg[s_ndbg].cmd), "%s", tok);
    s_ndbg++;
    return 1;
}

static int button(const char *name, ng_input_t *in) {
    static const struct { const char *n; int kind; uint8_t bit; } map[] = {
        {"up", 1, NG_UP}, {"down", 1, NG_DOWN}, {"left", 1, NG_LEFT}, {"right", 1, NG_RIGHT},
        {"a", 1, NG_A}, {"b", 1, NG_B}, {"c", 1, NG_C}, {"d", 1, NG_D},
        {"p2up", 2, NG_UP}, {"p2down", 2, NG_DOWN}, {"p2left", 2, NG_LEFT}, {"p2right", 2, NG_RIGHT},
        {"p2a", 2, NG_A}, {"p2b", 2, NG_B}, {"p2c", 2, NG_C}, {"p2d", 2, NG_D},
        {"start", 3, 0}, {"start2", 4, 0}, {"select", 5, 0}, {"coin", 6, 0}, {"coin2", 7, 0},
        {"service", 8, 0}, {"test", 9, 0},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(name, map[i].n)) continue;
        switch (map[i].kind) {
        case 1: in->p1 |= map[i].bit; break;
        case 2: in->p2 |= map[i].bit; break;
        case 3: in->start1 = 1; break;
        case 4: in->start2 = 1; break;
        case 5: in->select1 = 1; break;
        case 6: in->coin1 = 1; break;
        case 7: in->coin2 = 1; break;
        case 8: in->service = 1; break;
        case 9: in->test = 1; break;
        }
        return 0;
    }
    return -1;
}

static int load_script(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "error: cannot open %s\n", path); return -1; }
    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        long start, end;
        char buttons[200];
        int n = sscanf(line, "%ld-%ld %199s", &start, &end, buttons);
        if (n != 3) {
            if (sscanf(line, "%ld %199s", &start, buttons) != 2) continue;
            end = start + 5;   /* long enough for the BIOS to debounce a press */
        }
        if (!strchr(buttons, '+') && debug_accept(start, buttons)) continue;
        script_ev_t ev = {start, end, {0}};
        for (char *tok = strtok(buttons, "+"); tok; tok = strtok(NULL, "+"))
            if (button(tok, &ev.in)) {
                fprintf(stderr, "%s:%d: unknown button '%s'\n", path, lineno, tok);
                fclose(f); return -1;
            }
        s_script = (script_ev_t *)realloc(s_script, (size_t)(s_nscript + 1) * sizeof(*s_script));
        s_script[s_nscript++] = ev;
    }
    fclose(f);
    return 0;
}

static void script_input(long frame, ng_input_t *in) {
    for (int i = 0; i < s_nscript; i++) {
        const script_ev_t *e = &s_script[i];
        if (frame < e->start || frame > e->end) continue;
        in->p1 |= e->in.p1; in->p2 |= e->in.p2;
        in->start1 |= e->in.start1; in->start2 |= e->in.start2; in->select1 |= e->in.select1;
        in->coin1 |= e->in.coin1; in->coin2 |= e->in.coin2;
        in->service |= e->in.service; in->test |= e->in.test;
    }
}

/* ---- frame loop ---- */

static void run_until(int64_t t) {
    while (ng_cycles < t) {
        int64_t target = t, d = lspc_timer_deadline();
        if (d < target) target = d;
        if (target > ng_cycles) exec_run(target);
        if (ng_cycles >= lspc_timer_deadline()) lspc_timer_fire();
    }
}

static void run_frame(void) {
    int64_t fs = ng_frame_start;
    for (int line = 0; line < NG_LINES_PER_FRAME; line++) {
        int64_t ls = fs + (int64_t)line * NG_CLOCKS_PER_LINE;
        run_until(ls);
        if (line >= NG_FIRST_VISIBLE && line < NG_VBLANK_LINE)
            video_render_line(line - NG_FIRST_VISIBLE, s_fb + (line - NG_FIRST_VISIBLE) * ng_screen_w);
        if (line == NG_VBLANK_LINE) {
            run_until(ls + NG_VBLANK_HPOS * 2);
            lspc_vblank();
        }
    }
    ng_frame_start = fs + NG_CLOCKS_PER_FRAME;
    run_until(ng_frame_start);
    audio_sync();
}

static int s_mvs = 1;

/* One slot, named after the game, in the current directory. The width is
 * checked before anything is replaced: widescreen is patched into the ROM
 * at load, so a 16:9 picture cannot be restored into a 4:3 run. */
int ng_state_save(void) {
    FILE *f = fopen(slot_path(), "wb");
    int32_t w = ng_screen_w, h = NG_SCREEN_H;
    int ok;
    if (!f) { snprintf(s_status, sizeof(s_status), "cannot save"); return -1; }
    ok = fwrite("NGSTATE1", 1, 8, f) == 8
      && fwrite(&w, 4, 1, f) == 1
      && fwrite(&h, 4, 1, f) == 1
      && fwrite(&ng_cpu, sizeof(ng_cpu), 1, f) == 1
      && fwrite(&ng_cycles, sizeof(ng_cycles), 1, f) == 1
      && fwrite(&ng_frame_start, sizeof(ng_frame_start), 1, f) == 1
      && fwrite(&ng_irq_pending, 1, 1, f) == 1
      && bus_state_save(f) == 0
      && pal_state_save(f) == 0
      && lspc_state_save(f) == 0
      && audio_state_save(f) == 0;
    fclose(f);
    if (!ok) { snprintf(s_status, sizeof(s_status), "cannot save"); return -1; }
    snprintf(s_status, sizeof(s_status), "saved");
    fprintf(stderr, "[neogeorecomp] saved %s (%d wide)\n", slot_path(), ng_screen_w);
    return 0;
}

int ng_state_load(void) {
    FILE *f = fopen(slot_path(), "rb");
    char magic[8];
    int32_t w = 0, h = 0;
    if (!f) { snprintf(s_status, sizeof(s_status), "no save"); return -1; }
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "NGSTATE1", 8) ||
        fread(&w, 4, 1, f) != 1 || fread(&h, 4, 1, f) != 1) {
        fclose(f);
        snprintf(s_status, sizeof(s_status), "bad save");
        return -1;
    }
    if (w != ng_screen_w || h != NG_SCREEN_H) {
        fclose(f);
        snprintf(s_status, sizeof(s_status), "width differs");
        fprintf(stderr, "[neogeorecomp] save is %d wide and this run is %d; not loaded\n", w, ng_screen_w);
        return -2;
    }
    if (fread(&ng_cpu, sizeof(ng_cpu), 1, f) != 1 ||
        fread(&ng_cycles, sizeof(ng_cycles), 1, f) != 1 ||
        fread(&ng_frame_start, sizeof(ng_frame_start), 1, f) != 1 ||
        fread(&ng_irq_pending, 1, 1, f) != 1 ||
        bus_state_load(f) || pal_state_load(f) || lspc_state_load(f) || audio_state_load(f)) {
        fclose(f);
        snprintf(s_status, sizeof(s_status), "bad save");
        return -1;
    }
    fclose(f);
    snprintf(s_status, sizeof(s_status), "loaded");
    fprintf(stderr, "[neogeorecomp] loaded %s\n", slot_path());
    return 0;
}

int ng_debug_count(void) { return debug_nitems() + 2; }

void ng_debug_label(int index, char *buf, size_t n) {
    const ng_debug_item_t *d = debug_item(index);
    int v;
    if (!d) {
        snprintf(buf, n, "%s", index == debug_nitems() ? "Save slot" : "Load slot");
        return;
    }
    if (d->max < d->min) { snprintf(buf, n, "%s", d->label); return; }
    v = d->get ? d->get() : d->min;
    if (d->min == 0 && d->max == 1)
        snprintf(buf, n, "%-12s < %s >", d->label, v ? "on " : "off");
    else
        snprintf(buf, n, "%-12s < %d >", d->label, v);
}

int ng_debug_adjustable(int index) {
    const ng_debug_item_t *d = debug_item(index);
    return d && d->max >= d->min;
}

void ng_debug_action(int index, int dir) {
    const ng_debug_item_t *d;
    int v;
    if (index == debug_nitems()) { ng_state_save(); return; }
    if (index == debug_nitems() + 1) { ng_state_load(); return; }
    d = debug_item(index);
    if (!d) return;
    if (d->max < d->min) { if (dir == 0) debug_apply(d, 0, 0); return; }
    v = d->get ? d->get() : d->min;
    if (dir == 0) {
        if (d->min == 0 && d->max == 1) debug_apply(d, 1, !v);
        return;
    }
    v += dir;
    if (v < d->min) v = d->min;
    if (v > d->max) v = d->max;
    debug_apply(d, 1, v);
}

const char *ng_debug_status(void) { return s_status; }

static void power_on(void) {
    ng_cycles = 0;
    ng_frame_start = 0;
    bus_reset();
    lspc_reset();
    pal_select_bank(0);
    video_set_fix_bios(s_mvs);
    audio_select_bios_rom(s_mvs);
    audio_reset();
    exec_reset();
    ng_irq_pending = 4;        /* IRQ3 is asserted at power-on until the BIOS acks it */
}

int ng_main(int argc, char **argv, const ng_game_t *game,
            const ng_func_entry_t *funcs, size_t nfuncs) {
    options_t o;
    s_game = game;
    platform_early_init();
    if (parse_args(argc, argv, &o)) return 2;
    if (o.input && load_script(o.input)) return 1;
    /* The window's saved settings; the command line wins. Headless runs
     * never read them, so they behave the same everywhere. */
    if (!o.headless) {
        int w = window_settings_load(game->name);
        if (!o.width_set && w >= NG_SCREEN_W && w <= NG_MAX_SCREEN_W && !(w & 1)) ng_screen_w = w;
    }
    if (!o.bios_name) o.bios_name = game->bios ? game->bios : "sp-s2.sp1";

    size_t sz, bios_sz, sfix_sz, sm1_sz, lo_sz, srom_sz, m1_sz;
    uint8_t *prom = ng_load_prom(o.rom_path, game);
    uint8_t *bios = ng_read_file(o.bios_path, o.bios_name, &bios_sz);
    uint8_t *sfix = ng_read_file(o.bios_path, "sfix.sfix", &sfix_sz);
    uint8_t *sm1 = ng_read_file(o.bios_path, "sm1.sm1", &sm1_sz);
    uint8_t *lo = ng_read_file(o.bios_path, "000-lo.lo", &lo_sz);
    uint8_t *crom = ng_load_crom(o.rom_path, game);
    uint8_t *srom = ng_read_file(o.rom_path, game->srom, &srom_sz);
    uint8_t *m1 = ng_read_file(o.rom_path, game->m1, &m1_sz);
    if (!prom || !bios || !sfix || !sm1 || !lo || !crom || !srom || !m1) {
        fprintf(stderr, "error: missing ROM files; see docs/running.md for the expected set\n");
        return 1;
    }
    if (bios_sz < 0x20000) { fprintf(stderr, "error: %s is not a 128 KB system ROM\n", o.bios_name); return 1; }
    ng_swap16(bios, 0x20000);
    (void)sz;

    const uint8_t *vroms[NG_MAX_PARTS];
    size_t vsizes[NG_MAX_PARTS];
    int nv = 0;
    for (; nv < NG_MAX_PARTS && game->vrom[nv]; nv++) {
        uint8_t *v = ng_read_file(o.rom_path, game->vrom[nv], &vsizes[nv]);
        if (!v) return 1;
        vroms[nv] = v;
    }

    if (game->patch) game->patch(prom, game->prom_size, ng_screen_w);
    s_mvs = !game->aes;
    ng_board_t board = {prom, game->prom_size, bios, s_mvs};
    bus_init(&board);
    io_reset(s_mvs);
    video_load(crom, game->crom_size, srom, srom_sz, sfix, lo);
    if (ng_screen_w > NG_SCREEN_W) video_set_extend(game->extend);
    free(crom);
    audio_init(m1, m1_sz, sm1, vroms, vsizes, nv);
    exec_init(funcs, nfuncs, o.interp || nfuncs == 0);
    exec_set_verify(o.verify);
    if (platform_init(game->title, o.scale, o.headless, o.record)) return 1;
    if (o.mute) platform_mute();
    if (o.wav && platform_wav(o.wav)) return 1;

    fprintf(stderr, "[neogeorecomp] %s: %zu recompiled entry points%s\n", game->name, nfuncs,
            (o.interp || nfuncs == 0) ? ", interpreter only" : "");
    power_on();

    static int16_t samples[4096 * 2];
    for (long frame = 0; o.frames < 0 || frame < o.frames; frame++) {
        ng_input_t in;
        memset(&in, 0, sizeof(in));
        if (!platform_frame(frame == 0 ? NULL : s_fb, &in)) break;
        if (window_take_reset()) power_on();
        script_input(frame, &in);
        for (int i = 0; i < s_ndbg; i++)
            if (s_dbg[i].frame == frame) debug_run(s_dbg[i].cmd);
        if (s_game->before_frame) s_game->before_frame();
        io_set_inputs(&in);
        run_frame();
        platform_audio(samples, audio_take_samples(samples, 4096));
        for (int i = 0; i < o.nshots; i++)
            if (o.shots[i].frame == frame) {
                if (write_png(o.shots[i].path, s_fb, ng_screen_w, NG_SCREEN_H))
                    fprintf(stderr, "error: cannot write %s\n", o.shots[i].path);
                else
                    fprintf(stderr, "[neogeorecomp] frame %ld -> %s\n", frame, o.shots[i].path);
            }
    }
    platform_frame(s_fb, &(ng_input_t){0});

    exec_report();
    if (o.dump_misses) exec_dump_misses(o.dump_misses);
    platform_shutdown();
    video_shutdown();
    audio_shutdown();
    return 0;
}
