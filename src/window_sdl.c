/*
 * window_sdl.c — the SDL2 window: presentation, sound, keyboard, gamepads,
 * frame pacing, and the pause menu with the player's settings.
 * Only built when SDL2 is found; see platform.c for the headless stubs.
 *
 * Default keys: player 1 arrows and Z/X/C/V = A/B/C/D, 1 = start,
 * 3 = select; player 2 R/F/D/G and A/S/Q/W, 2 = start, 4 = select
 * (MAME's layout); 5/6 = coin 1/2, 9 = service, F2 = test switch.
 * Esc, or a gamepad's Guide button, opens the menu.
 *
 * Gamepads: the first connected plays player 1, the second player 2.
 * D-pad or left stick move; the face buttons South/East/West/North
 * (Xbox A/B/X/Y) are A/B/C/D, Start is start, Back is select.
 *
 * Settings live in SDL's per-user folder for the game (on macOS,
 * ~/Library/Application Support/neogeorecomp/<game>/settings.ini). Only
 * the window reads them; headless runs stay the same on every machine.
 */
#include "ng_internal.h"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font8x8_basic.h"

static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture *s_tex;
static uint64_t s_next_tick;
static SDL_AudioDeviceID s_audio;
static SDL_GameController *s_pad[2];
static int s_reset;

/* ---- settings ---- */

enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_A, K_B, K_C, K_D, K_START, K_SELECT, NKEYS };
static const char *const k_key_names[NKEYS] = {"up", "down", "left", "right", "a", "b", "c", "d", "start", "select"};
static const char *const k_key_labels[NKEYS] = {"Up", "Down", "Left", "Right", "A", "B", "C", "D", "Start", "Select"};
static const SDL_Scancode k_default_keys[2][NKEYS] = {
    {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
     SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_1, SDL_SCANCODE_3},
    {SDL_SCANCODE_R, SDL_SCANCODE_F, SDL_SCANCODE_D, SDL_SCANCODE_G,
     SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_2, SDL_SCANCODE_4},
};

static struct {
    int volume;         /* percent, in steps of 10 */
    int fullscreen, scanlines;
    int width;          /* the output width for the next start; 0 = the game's default */
    SDL_Scancode keys[2][NKEYS];
} s_set;
static char s_set_path[1024];
static int s_set_loaded;

static void settings_defaults(void) {
    s_set.volume = 100;
    s_set.fullscreen = 0;
    s_set.scanlines = 0;
    s_set.width = 0;
    memcpy(s_set.keys, k_default_keys, sizeof(s_set.keys));
}

int window_settings_load(const char *game) {
    settings_defaults();
    s_set_loaded = 1;
    char *dir = SDL_GetPrefPath("neogeorecomp", game);
    if (!dir) return 0;
    snprintf(s_set_path, sizeof(s_set_path), "%ssettings.ini", dir);
    SDL_free(dir);
    FILE *f = fopen(s_set_path, "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = 0;
        if (!strcmp(line, "volume")) s_set.volume = atoi(val) < 0 ? 0 : atoi(val) > 100 ? 100 : atoi(val);
        else if (!strcmp(line, "fullscreen")) s_set.fullscreen = atoi(val) != 0;
        else if (!strcmp(line, "scanlines")) s_set.scanlines = atoi(val) != 0;
        else if (!strcmp(line, "width")) s_set.width = atoi(val);
        else
            for (int p = 0; p < 2; p++)
                for (int k = 0; k < NKEYS; k++) {
                    char name[32];
                    snprintf(name, sizeof(name), "p%d_%s", p + 1, k_key_names[k]);
                    SDL_Scancode sc = SDL_GetScancodeFromName(val);
                    if (!strcmp(line, name) && sc != SDL_SCANCODE_UNKNOWN) s_set.keys[p][k] = sc;
                }
    }
    fclose(f);
    return s_set.width;
}

static void settings_save(void) {
    if (!s_set_path[0]) return;
    FILE *f = fopen(s_set_path, "w");
    if (!f) { fprintf(stderr, "warning: cannot write %s\n", s_set_path); return; }
    fprintf(f, "volume=%d\nfullscreen=%d\nscanlines=%d\nwidth=%d\n",
            s_set.volume, s_set.fullscreen, s_set.scanlines, s_set.width);
    for (int p = 0; p < 2; p++)
        for (int k = 0; k < NKEYS; k++)
            fprintf(f, "p%d_%s=%s\n", p + 1, k_key_names[k], SDL_GetScancodeName(s_set.keys[p][k]));
    fclose(f);
}

int window_take_reset(void) {
    int r = s_reset;
    s_reset = 0;
    return r;
}

/* ---- window ---- */

int window_open(const char *title, int scale) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "error: SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    if (!s_set_loaded) settings_defaults();
    s_win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             ng_screen_w * scale, NG_SCREEN_H * scale, SDL_WINDOW_RESIZABLE);
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                              ng_screen_w, NG_SCREEN_H);
    if (s_set.fullscreen) SDL_SetWindowFullscreen(s_win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    s_next_tick = SDL_GetPerformanceCounter();
    /* Sound at the YM2610's own rate; SDL converts it for the device. A
     * missing audio device leaves the game silent, not stopped. */
    SDL_AudioSpec want = {0};
    want.freq = NG_AUDIO_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
        s_audio = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (!s_audio) fprintf(stderr, "warning: no sound: %s\n", SDL_GetError());
    /* Pads already connected arrive as device-added events. */
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        fprintf(stderr, "warning: no gamepads: %s\n", SDL_GetError());
    return 0;
}

static void pad_added(int index) {
    for (int p = 0; p < 2; p++) {
        if (s_pad[p]) continue;
        s_pad[p] = SDL_GameControllerOpen(index);
        if (s_pad[p]) fprintf(stderr, "[neogeorecomp] gamepad \"%s\" is player %d\n", SDL_GameControllerName(s_pad[p]), p + 1);
        return;
    }
}

static void pad_removed(SDL_JoystickID id) {
    for (int p = 0; p < 2; p++)
        if (s_pad[p] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pad[p])) == id) {
            SDL_GameControllerClose(s_pad[p]);
            s_pad[p] = NULL;
        }
}

static void read_pad(SDL_GameController *c, uint8_t *pad, uint8_t *start, uint8_t *select) {
    const int dead = 16384;
    int x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    int y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP) || y < -dead) *pad |= NG_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || y > dead) *pad |= NG_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || x < -dead) *pad |= NG_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || x > dead) *pad |= NG_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A)) *pad |= NG_A;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B)) *pad |= NG_B;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X)) *pad |= NG_C;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y)) *pad |= NG_D;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START)) *start = 1;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK)) *select = 1;
}

/* The frame loop and the sound card both run at the board's rates, so the
 * queue stays level; it is kept between about 30 and 120 ms, dropping a
 * frame's samples when it runs long (after a stall) and padding with
 * silence when it runs dry. */
void window_audio(const int16_t *samples, size_t frames) {
    if (!s_audio) return;
    const Uint32 bytes_per_ms = NG_AUDIO_RATE * 4 / 1000;
    Uint32 queued = SDL_GetQueuedAudioSize(s_audio);
    if (queued > 120 * bytes_per_ms) return;
    if (queued < 30 * bytes_per_ms) {
        static const int16_t silence[2 * 2048];   /* 37 ms */
        SDL_QueueAudio(s_audio, silence, (Uint32)(30 * bytes_per_ms - queued) & ~3u);
    }
    static int16_t scaled[4096 * 2];
    if (frames > 4096) frames = 4096;
    for (size_t i = 0; i < frames * 2; i++) scaled[i] = (int16_t)(samples[i] * s_set.volume / 100);
    SDL_QueueAudio(s_audio, scaled, (Uint32)(frames * 4));
    SDL_PauseAudioDevice(s_audio, 0);
}

static void read_keys(ng_input_t *in) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    static const uint8_t bit[8] = {NG_UP, NG_DOWN, NG_LEFT, NG_RIGHT, NG_A, NG_B, NG_C, NG_D};
    for (int i = 0; i < 8; i++) {
        if (k[s_set.keys[0][i]]) in->p1 |= bit[i];
        if (k[s_set.keys[1][i]]) in->p2 |= bit[i];
    }
    in->start1 = k[s_set.keys[0][K_START]];
    in->select1 = k[s_set.keys[0][K_SELECT]];
    in->start2 = k[s_set.keys[1][K_START]];
    in->select2 = k[s_set.keys[1][K_SELECT]];
    if (s_pad[0]) read_pad(s_pad[0], &in->p1, &in->start1, &in->select1);
    if (s_pad[1]) read_pad(s_pad[1], &in->p2, &in->start2, &in->select2);
    in->coin1 = k[SDL_SCANCODE_5];
    in->coin2 = k[SDL_SCANCODE_6];
    in->service = k[SDL_SCANCODE_9];
    in->test = k[SDL_SCANCODE_F2];
}

/* The picture, scaled to fit the window with its aspect kept, and with
 * scanlines: every source line's lower half darkened, as a CRT's gaps
 * between lines look. */
static void present(const uint32_t *fb) {
    SDL_UpdateTexture(s_tex, NULL, fb, ng_screen_w * 4);
    int ow, oh;
    SDL_GetRendererOutputSize(s_ren, &ow, &oh);
    SDL_Rect dst;
    if ((int64_t)ow * NG_SCREEN_H > (int64_t)oh * ng_screen_w) {
        dst.h = oh; dst.w = oh * ng_screen_w / NG_SCREEN_H;
    } else {
        dst.w = ow; dst.h = ow * NG_SCREEN_H / ng_screen_w;
    }
    dst.x = (ow - dst.w) / 2; dst.y = (oh - dst.h) / 2;
    SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, &dst);
    if (s_set.scanlines && dst.h >= NG_SCREEN_H * 2) {
        static SDL_Rect r[NG_SCREEN_H];
        for (int y = 0; y < NG_SCREEN_H; y++) {
            int top = dst.y + y * dst.h / NG_SCREEN_H, bottom = dst.y + (y + 1) * dst.h / NG_SCREEN_H;
            r[y].x = dst.x; r[y].w = dst.w;
            r[y].y = top + (bottom - top + 1) / 2; r[y].h = bottom - r[y].y;
        }
        SDL_SetRenderDrawBlendMode(s_ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 96);
        SDL_RenderFillRects(s_ren, r, NG_SCREEN_H);
    }
    SDL_RenderPresent(s_ren);
}

/* ---- the pause menu ---- */

enum { M_RESUME, M_VOLUME, M_FULLSCREEN, M_SCANLINES, M_SCREEN, M_KEYS1, M_KEYS2, M_RESET, M_QUIT, M_COUNT };
enum { EV_NONE, EV_UP, EV_DOWN, EV_LEFT, EV_RIGHT, EV_OK, EV_BACK };

static uint32_t s_menu_fb[NG_MAX_SCREEN_W * NG_SCREEN_H];

static void text(int x, int y, const char *s, uint32_t colour) {
    for (; *s; s++, x += 8)
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++)
                if ((font8x8_basic[(unsigned char)*s & 0x7F][r] >> c) & 1) {
                    int px = x + c, py = y + r;
                    if (px >= 0 && px < ng_screen_w && py >= 0 && py < NG_SCREEN_H)
                        s_menu_fb[py * ng_screen_w + px] = colour;
                }
}

static void box(int x, int y, int w, int h, uint32_t colour) {
    for (int py = y; py < y + h; py++)
        for (int px = x; px < x + w; px++)
            if (px >= 0 && px < ng_screen_w && py >= 0 && py < NG_SCREEN_H)
                s_menu_fb[py * ng_screen_w + px] = colour;
}

/* The game's last frame, dimmed, under a panel with the lines; the
 * selected line is highlighted. */
static void draw_menu(const uint32_t *game, const char *title, char lines[][40], int n, int sel, const char *hint) {
    for (int i = 0; i < ng_screen_w * NG_SCREEN_H; i++) {
        uint32_t c = game[i];
        s_menu_fb[i] = 0xFF000000u | (((c >> 16) & 0xFF) / 3 << 16) | (((c >> 8) & 0xFF) / 3 << 8) | ((c & 0xFF) / 3);
    }
    int w = 232, h = 32 + n * 12 + 14, x = (ng_screen_w - w) / 2, y = (NG_SCREEN_H - h) / 2;
    box(x, y, w, h, 0xFF101828u);
    box(x, y, w, 1, 0xFF5070C0u); box(x, y + h - 1, w, 1, 0xFF5070C0u);
    box(x, y, 1, h, 0xFF5070C0u); box(x + w - 1, y, 1, h, 0xFF5070C0u);
    text(x + (w - (int)strlen(title) * 8) / 2, y + 8, title, 0xFFFFD040u);
    for (int i = 0; i < n; i++) {
        int ly = y + 26 + i * 12;
        if (i == sel) box(x + 4, ly - 2, w - 8, 12, 0xFF2C4A90u);
        text(x + 12, ly, lines[i], i == sel ? 0xFFFFFFFFu : 0xFFB0B8C8u);
    }
    if (hint) text(x + (w - (int)strlen(hint) * 8) / 2, y + h - 12, hint, 0xFF8090A8u);
    present(s_menu_fb);
}

static int menu_event(const SDL_Event *e) {
    if (e->type == SDL_KEYDOWN) {
        switch (e->key.keysym.scancode) {
        case SDL_SCANCODE_UP: return EV_UP;
        case SDL_SCANCODE_DOWN: return EV_DOWN;
        case SDL_SCANCODE_LEFT: return EV_LEFT;
        case SDL_SCANCODE_RIGHT: return EV_RIGHT;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: return EV_OK;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE: return EV_BACK;
        default: return EV_NONE;
        }
    }
    if (e->type == SDL_CONTROLLERBUTTONDOWN) {
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return EV_UP;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return EV_DOWN;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return EV_LEFT;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return EV_RIGHT;
        case SDL_CONTROLLER_BUTTON_A: return EV_OK;
        case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_GUIDE: return EV_BACK;
        default: return EV_NONE;
        }
    }
    return EV_NONE;
}

/* Remaps one player's keys: OK on a line waits for the next key. */
static int keys_menu(const uint32_t *game, int player) {
    int sel = 0, waiting = 0;
    char title[40];
    snprintf(title, sizeof(title), "PLAYER %d KEYS", player + 1);
    for (;;) {
        char lines[NKEYS + 2][40];
        for (int k = 0; k < NKEYS; k++)
            snprintf(lines[k], 40, "%-7s %s", k_key_labels[k],
                     waiting && sel == k ? "press a key..." : SDL_GetScancodeName(s_set.keys[player][k]));
        snprintf(lines[NKEYS], 40, "Defaults");
        snprintf(lines[NKEYS + 1], 40, "Back");
        draw_menu(game, title, lines, NKEYS + 2, sel, waiting ? "Esc cancels" : "Enter changes a key");
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 100)) continue;
        if (e.type == SDL_QUIT) return 0;
        if (e.type == SDL_CONTROLLERDEVICEADDED) pad_added(e.cdevice.which);
        if (e.type == SDL_CONTROLLERDEVICEREMOVED) pad_removed(e.cdevice.which);
        if (waiting) {
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.scancode != SDL_SCANCODE_ESCAPE) s_set.keys[player][sel] = e.key.keysym.scancode;
                waiting = 0;
            }
            continue;
        }
        switch (menu_event(&e)) {
        case EV_UP: sel = (sel + NKEYS + 1) % (NKEYS + 2); break;
        case EV_DOWN: sel = (sel + 1) % (NKEYS + 2); break;
        case EV_BACK: return 1;
        case EV_OK:
            if (sel < NKEYS) waiting = 1;
            else if (sel == NKEYS) memcpy(s_set.keys[player], k_default_keys[player], sizeof(s_set.keys[player]));
            else return 1;
            break;
        default: break;
        }
    }
}

/* Runs the menu over the paused game; returns 0 to quit. */
static int menu(const uint32_t *game) {
    int sel = 0, width_at_start = ng_screen_w;
    if (s_audio) { SDL_PauseAudioDevice(s_audio, 1); SDL_ClearQueuedAudio(s_audio); }
    for (;;) {
        char lines[M_COUNT][40];
        int next_width = s_set.width ? s_set.width : width_at_start;
        snprintf(lines[M_RESUME], 40, "Resume");
        snprintf(lines[M_VOLUME], 40, "Volume       < %3d%% >", s_set.volume);
        snprintf(lines[M_FULLSCREEN], 40, "Fullscreen   < %s >", s_set.fullscreen ? "on " : "off");
        snprintf(lines[M_SCANLINES], 40, "Scanlines    < %s >", s_set.scanlines ? "on " : "off");
        snprintf(lines[M_SCREEN], 40, "Screen       < %s >%s", next_width > NG_SCREEN_W ? "16:9" : "4:3 ",
                 next_width != width_at_start ? " *" : "");
        snprintf(lines[M_KEYS1], 40, "Player 1 keys");
        snprintf(lines[M_KEYS2], 40, "Player 2 keys");
        snprintf(lines[M_RESET], 40, "Reset");
        snprintf(lines[M_QUIT], 40, "Quit");
        draw_menu(game, "PAUSED", lines, M_COUNT, sel,
                  next_width != width_at_start ? "* after a restart" : "Esc resumes");
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 100)) continue;
        if (e.type == SDL_QUIT) { settings_save(); return 0; }
        if (e.type == SDL_CONTROLLERDEVICEADDED) pad_added(e.cdevice.which);
        if (e.type == SDL_CONTROLLERDEVICEREMOVED) pad_removed(e.cdevice.which);
        int ev = menu_event(&e), step = ev == EV_LEFT ? -1 : ev == EV_RIGHT ? 1 : ev == EV_OK ? 1 : 0;
        if (ev == EV_UP) sel = (sel + M_COUNT - 1) % M_COUNT;
        else if (ev == EV_DOWN) sel = (sel + 1) % M_COUNT;
        else if (ev == EV_BACK) break;
        else if (step) {
            switch (sel) {
            case M_RESUME: if (ev == EV_OK) goto done; break;
            case M_VOLUME:
                s_set.volume += step * 10;
                s_set.volume = s_set.volume < 0 ? 0 : s_set.volume > 100 ? 100 : s_set.volume;
                break;
            case M_FULLSCREEN:
                s_set.fullscreen = !s_set.fullscreen;
                SDL_SetWindowFullscreen(s_win, s_set.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                break;
            case M_SCANLINES: s_set.scanlines = !s_set.scanlines; break;
            case M_SCREEN: s_set.width = next_width > NG_SCREEN_W ? NG_SCREEN_W : 400; break;
            case M_KEYS1: case M_KEYS2:
                if (ev == EV_OK && !keys_menu(game, sel == M_KEYS1 ? 0 : 1)) { settings_save(); return 0; }
                break;
            case M_RESET: if (ev == EV_OK) { s_reset = 1; goto done; } break;
            case M_QUIT: if (ev == EV_OK) { settings_save(); return 0; } break;
            }
        }
    }
done:
    settings_save();
    s_next_tick = SDL_GetPerformanceCounter();
    return 1;
}

bool window_frame(const uint32_t *fb, ng_input_t *in) {
    static uint32_t last[NG_MAX_SCREEN_W * NG_SCREEN_H];
    if (fb) memcpy(last, fb, sizeof(uint32_t) * (size_t)ng_screen_w * NG_SCREEN_H);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return false;
        if (e.type == SDL_CONTROLLERDEVICEADDED) pad_added(e.cdevice.which);
        if (e.type == SDL_CONTROLLERDEVICEREMOVED) pad_removed(e.cdevice.which);
        if ((e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE && !e.key.repeat) ||
            (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE))
            if (!menu(last)) return false;
    }
    read_keys(in);
    if (fb) present(fb);
    /* Pace to the MVS refresh rate, 59.1856 Hz. */
    uint64_t freq = SDL_GetPerformanceFrequency();
    s_next_tick += (uint64_t)((double)freq / 59.185606);
    uint64_t now = SDL_GetPerformanceCounter();
    if (now < s_next_tick) SDL_Delay((Uint32)((s_next_tick - now) * 1000 / freq));
    else if (now - s_next_tick > freq / 10) s_next_tick = now;
    return true;
}

void window_close(void) {
    settings_save();
    for (int p = 0; p < 2; p++) if (s_pad[p]) SDL_GameControllerClose(s_pad[p]);
    if (s_audio) SDL_CloseAudioDevice(s_audio);
    if (s_tex) SDL_DestroyTexture(s_tex);
    if (s_ren) SDL_DestroyRenderer(s_ren);
    if (s_win) SDL_DestroyWindow(s_win);
    SDL_Quit();
}
