/*
 * platform.c — host output: headless recording, PNG screenshots, crash reports.
 *
 * --headless opens no window at all (it works over RDP and in CI) and runs
 * unpaced. --record pipes raw frames to ffmpeg, which must be on PATH.
 * The window itself is window_sdl.c.
 */
#include "ng_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

/* On a crash, print a symbolized stack so a report is actionable. */
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(proc, NULL, TRUE);
    fprintf(stderr, "\n*** crash: exception %08lX at %p\n",
            ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 f;
    memset(&f, 0, sizeof(f));
    f.AddrPC.Offset = ctx.Rip; f.AddrPC.Mode = AddrModeFlat;
    f.AddrFrame.Offset = ctx.Rbp; f.AddrFrame.Mode = AddrModeFlat;
    f.AddrStack.Offset = ctx.Rsp; f.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 32; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &f, &ctx, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL) || !f.AddrPC.Offset)
            break;
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line = {sizeof(line)};
        DWORD ldisp = 0;
        const char *name = SymFromAddr(proc, f.AddrPC.Offset, &disp, sym) ? sym->Name : "?";
        if (SymGetLineFromAddr64(proc, f.AddrPC.Offset, &ldisp, &line))
            fprintf(stderr, "  %s  %s:%lu\n", name, line.FileName, line.LineNumber);
        else
            fprintf(stderr, "  %s+0x%llx\n", name, (unsigned long long)disp);
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

static int s_headless;

void platform_early_init(void) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_handler);
#endif
}
static FILE *s_rec;

/* The window lives in window_sdl.c; builds without SDL2 get these stubs
 * and run headless only. */
#ifndef NG_HAVE_SDL
int  window_open(const char *title, int scale) {
    (void)title; (void)scale;
    fprintf(stderr, "error: built without SDL2; run with --headless\n");
    return -1;
}
bool window_frame(const uint32_t *fb, ng_input_t *in) { (void)fb; (void)in; return true; }
void window_audio(const int16_t *samples, size_t frames) { (void)samples; (void)frames; }
int  window_settings_load(const char *game) { (void)game; return 0; }
int  window_take_reset(void) { return 0; }
void window_close(void) {}
#endif

int platform_init(const char *title, int scale, int headless, const char *record_path) {
    s_headless = headless;
    if (record_path) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -loglevel error -y -f rawvideo -pix_fmt bgra -s %dx%d -r 59.185606 -i - "
                 "-vf scale=%d:%d:flags=neighbor -c:v libx264 -pix_fmt yuv420p -crf 18 \"%s\"",
                 ng_screen_w, NG_SCREEN_H, ng_screen_w * 2, NG_SCREEN_H * 2, record_path);
        s_rec = popen(cmd, "wb");
        if (!s_rec) { fprintf(stderr, "error: cannot start ffmpeg for --record\n"); return -1; }
    }
    return headless ? 0 : window_open(title, scale);
}

bool platform_frame(const uint32_t *fb, ng_input_t *in) {
    if (fb && s_rec) fwrite(fb, 4, (size_t)ng_screen_w * NG_SCREEN_H, s_rec);
    return s_headless ? true : window_frame(fb, in);
}

/* ---- audio: the window, and --wav ---- */

static FILE *s_wav;
static uint32_t s_wav_bytes;
static int s_mute;

static void put16le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32le(uint8_t *p, uint32_t v) { put16le(p, v); put16le(p + 2, v >> 16); }

static void wav_header(FILE *f, uint32_t data_bytes) {
    uint8_t h[44];
    memcpy(h, "RIFF", 4); put32le(h + 4, 36 + data_bytes); memcpy(h + 8, "WAVEfmt ", 8);
    put32le(h + 16, 16); put16le(h + 20, 1); put16le(h + 22, 2);
    put32le(h + 24, NG_AUDIO_RATE); put32le(h + 28, NG_AUDIO_RATE * 4); put16le(h + 32, 4); put16le(h + 34, 16);
    memcpy(h + 36, "data", 4); put32le(h + 40, data_bytes);
    fwrite(h, 1, 44, f);
}

int platform_wav(const char *path) {
    s_wav = fopen(path, "wb");
    if (!s_wav) { fprintf(stderr, "error: cannot write %s\n", path); return -1; }
    wav_header(s_wav, 0);
    return 0;
}

void platform_mute(void) { s_mute = 1; }

void platform_audio(const int16_t *samples, size_t frames) {
    if (s_wav) {
        for (size_t i = 0; i < frames * 2; i++) {   /* WAV is little-endian */
            uint8_t b[2];
            put16le(b, (uint16_t)samples[i]);
            fwrite(b, 1, 2, s_wav);
        }
        s_wav_bytes += (uint32_t)(frames * 4);
    }
    if (!s_headless && !s_mute) window_audio(samples, frames);
}

void platform_shutdown(void) {
    if (s_rec) { pclose(s_rec); s_rec = NULL; }
    if (s_wav) { fseek(s_wav, 0, SEEK_SET); wav_header(s_wav, s_wav_bytes); fclose(s_wav); s_wav = NULL; }
    if (!s_headless) window_close();
}

/* ---- PNG writer (stored deflate blocks; no zlib dependency) ---- */

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    uint8_t hdr[8];
    put32(hdr, (uint32_t)n);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t c = crc32_update(0, hdr + 4, 4);
    c = crc32_update(c, data, n);
    uint8_t cb[4];
    put32(cb, c);
    fwrite(cb, 1, 4, f);
}

int write_png(const char *path, const uint32_t *argb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    put32(ihdr, (uint32_t)w); put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);

    size_t raw_n = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw = (uint8_t *)malloc(raw_n);
    uint8_t *p = raw;
    for (int y = 0; y < h; y++) {
        *p++ = 0;
        for (int x = 0; x < w; x++) {
            uint32_t c = argb[y * w + x];
            *p++ = (uint8_t)(c >> 16); *p++ = (uint8_t)(c >> 8); *p++ = (uint8_t)c;
        }
    }
    size_t blocks = (raw_n + 65534) / 65535;
    size_t z_n = 2 + raw_n + blocks * 5 + 4;
    uint8_t *z = (uint8_t *)malloc(z_n), *q = z;
    *q++ = 0x78; *q++ = 0x01;
    uint32_t s1 = 1, s2 = 0;
    for (size_t off = 0; off < raw_n; off += 65535) {
        size_t len = raw_n - off < 65535 ? raw_n - off : 65535;
        *q++ = (off + len == raw_n) ? 1 : 0;
        *q++ = (uint8_t)len; *q++ = (uint8_t)(len >> 8);
        *q++ = (uint8_t)~len; *q++ = (uint8_t)(~len >> 8);
        memcpy(q, raw + off, len);
        for (size_t i = 0; i < len; i++) { s1 = (s1 + raw[off + i]) % 65521; s2 = (s2 + s1) % 65521; }
        q += len;
    }
    put32(q, (s2 << 16) | s1); q += 4;
    chunk(f, "IDAT", z, (size_t)(q - z));
    chunk(f, "IEND", NULL, 0);
    free(raw); free(z);
    fclose(f);
    return 0;
}

/* ---- reading indexed PNGs (ng_read_indexed_png) ---- */

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

uint8_t *ng_read_indexed_png(const char *path, int *w_out, int *h_out, uint8_t rgb[768]) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "error: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    size_t n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = (uint8_t *)malloc(n ? n : 1);
    if (fread(file, 1, n, f) != n) n = 0;
    fclose(f);
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    uint8_t *idat = NULL, *out = NULL;
    size_t idat_n = 0;
    int w = 0, h = 0, depth = 0, ok = n >= 8 && !memcmp(file, sig, 8);
    if (rgb) memset(rgb, 0, 768);
    for (size_t at = 8; ok && at + 12 <= n;) {
        uint32_t len = get32(file + at);
        const uint8_t *type = file + at + 4, *data = file + at + 8;
        if (at + 12 + len > n) { ok = 0; break; }
        if (!memcmp(type, "IHDR", 4)) {
            w = (int)get32(data); h = (int)get32(data + 4); depth = data[8];
            if (data[9] != 3 || data[12] != 0 || depth > 8) {
                fprintf(stderr, "error: %s is not an indexed, non-interlaced PNG\n", path);
                ok = 0;
            }
        } else if (!memcmp(type, "PLTE", 4)) {
            if (rgb) memcpy(rgb, data, len < 768 ? len : 768);
        } else if (!memcmp(type, "IDAT", 4)) {
            idat = (uint8_t *)realloc(idat, idat_n + len);
            memcpy(idat + idat_n, data, len);
            idat_n += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        at += 12 + len;
    }
    int stride = (w * depth + 7) / 8, raw_n = 0;
    uint8_t *raw = ok && idat ? (uint8_t *)stbi_zlib_decode_malloc_guesssize_headerflag(
        (const char *)idat, (int)idat_n, h * (stride + 1), &raw_n, 1) : NULL;
    if (raw && raw_n >= h * (stride + 1)) {
        out = (uint8_t *)malloc((size_t)w * h);
        uint8_t *prev = (uint8_t *)calloc((size_t)stride, 1), *cur = (uint8_t *)malloc((size_t)stride);
        for (int y = 0; y < h; y++) {
            const uint8_t *line = raw + y * (stride + 1);
            for (int i = 0; i < stride; i++) {
                int a = i ? cur[i - 1] : 0, b = prev[i], c = i ? prev[i - 1] : 0, v = line[1 + i];
                switch (line[0]) {
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += paeth(a, b, c); break;
                }
                cur[i] = (uint8_t)v;
            }
            for (int x = 0; x < w; x++) {
                int bit = x * depth;
                out[y * w + x] = (uint8_t)((cur[bit >> 3] >> (8 - depth - (bit & 7))) & ((1 << depth) - 1));
            }
            memcpy(prev, cur, (size_t)stride);
        }
        free(prev); free(cur);
        *w_out = w; *h_out = h;
    } else if (ok) {
        fprintf(stderr, "error: cannot decode %s\n", path);
    }
    free(raw); free(idat); free(file);
    return out;
}
