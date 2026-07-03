/*
 * lua_main.c - Lua 5.4 ELF app for BreezyBox
 *
 * Pico-8-style graphics/input API over the breezybox firmware symbols.
 * Entry point: app_main(int argc, char **argv)
 *   argc == 1  -> interactive REPL
 *   argc >= 2  -> luaL_dofile(L, argv[1])
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

/* Reference to luaopen_base to prevent it from being stripped from liblua.a */
extern int luaopen_base(lua_State *L);
static void *dummy_luaopen_base_ref = (void *)luaopen_base;

/* ========== Firmware symbol forward declarations ========== */

void     rgb_display_set_mode(int mode);
uint8_t *rgb_display_get_framebuffer(void);
void     rgb_display_set_vga_palette(const uint16_t *pal);
void     rgb_display_refresh_palette(void);
void     rgb_display_wait_vsync(void);
void     rgb_display_snapshot(void);
void     rgb_gfx_clear(uint8_t c);
void     rgb_gfx_pixel(int x, int y, uint8_t c);
void     rgb_gfx_hline(int x, int y, int w, uint8_t c);
void     rgb_gfx_vline(int x, int y, int h, uint8_t c);
void     rgb_gfx_rect(int x, int y, int w, int h, uint8_t c);
void     rgb_gfx_rectfill(int x, int y, int w, int h, uint8_t c);
void     rgb_gfx_blit(const uint8_t *data, int x, int y, int w, int h, int stride, int tc);
int      bt_keyboard_is_pressed(int keycode);
void     vterm_get_size(int *rows, int *cols);

typedef unsigned int TickType_t;
TickType_t xTaskGetTickCount(void);
void       vTaskDelay(unsigned int ticks);

void    *heap_caps_malloc(size_t size, uint32_t caps);
void    *heap_caps_realloc(void *ptr, size_t size, uint32_t caps);
void     heap_caps_free(void *ptr);
size_t   heap_caps_get_free_size(uint32_t caps);

extern int rand(void);
extern void srand(unsigned int seed);

#define MALLOC_CAP_SPIRAM   (1 << 3)
#define MALLOC_CAP_8BIT     (1 << 2)

/* ========== Custom PSRAM Lua allocator ========== */

static void *lua_psram_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud; (void)osize;
    if (nsize == 0) {
        if (ptr) heap_caps_free(ptr);
        return NULL;
    }
    if (ptr == NULL) {
        return heap_caps_malloc(nsize, MALLOC_CAP_SPIRAM);
    }
    return heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM);
}

/* ========== Bresenham line helper ========== */

static void gfx_line(int x0, int y0, int x1, int y1, uint8_t c) {
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    while (1) {
        rgb_gfx_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* ========== Pico-8-style Lua API ========== */

static int l_cls(lua_State *L) {
    uint8_t c = (uint8_t)luaL_optinteger(L, 1, 0);
    rgb_gfx_clear(c);
    return 0;
}

static int l_mode(lua_State *L) {
    int m = (int)luaL_checkinteger(L, 1);
    static const int mode_map[] = { 3, 0x13, 0x80 };
    int hw_mode;
    if (m >= 0 && m <= 2) {
        hw_mode = mode_map[m];
    } else {
        hw_mode = m;
    }
    rgb_display_set_mode(hw_mode);
    return 0;
}

static int l_pset(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    uint8_t c = (uint8_t)luaL_checkinteger(L, 3);
    rgb_gfx_pixel(x, y, c);
    return 0;
}

static int l_line(lua_State *L) {
    int x0 = (int)luaL_checkinteger(L, 1);
    int y0 = (int)luaL_checkinteger(L, 2);
    int x1 = (int)luaL_checkinteger(L, 3);
    int y1 = (int)luaL_checkinteger(L, 4);
    uint8_t c = (uint8_t)luaL_checkinteger(L, 5);
    gfx_line(x0, y0, x1, y1, c);
    return 0;
}

static int l_rect(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    uint8_t c = (uint8_t)luaL_checkinteger(L, 5);
    rgb_gfx_rect(x, y, w, h, c);
    return 0;
}

static int l_rectfill(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    uint8_t c = (uint8_t)luaL_checkinteger(L, 5);
    rgb_gfx_rectfill(x, y, w, h, c);
    return 0;
}

static int l_spr(lua_State *L) {
    size_t datalen;
    const char *data = luaL_checklstring(L, 1, &datalen);
    int x      = (int)luaL_checkinteger(L, 2);
    int y      = (int)luaL_checkinteger(L, 3);
    int w      = (int)luaL_checkinteger(L, 4);
    int h      = (int)luaL_checkinteger(L, 5);
    int stride = (int)luaL_optinteger(L, 6, w);
    int tc     = (int)luaL_optinteger(L, 7, -1);
    rgb_gfx_blit((const uint8_t *)data, x, y, w, h, stride, tc);
    return 0;
}

static int l_pal(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    uint16_t pal[256];
    for (int i = 0; i < 256; i++) {
        lua_rawgeti(L, 1, i + 1);
        pal[i] = (uint16_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    rgb_display_set_vga_palette(pal);
    return 0;
}

static int l_flip(lua_State *L) {
    (void)L;
    rgb_display_snapshot();
    rgb_display_wait_vsync();
    return 0;
}

static int l_btn(lua_State *L) {
    int k = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, bt_keyboard_is_pressed(k));
    return 1;
}

static int l_time(lua_State *L) {
    TickType_t ticks = xTaskGetTickCount();
    lua_pushnumber(L, (lua_Number)ticks * 10 / 1000.0);
    return 1;
}

static int l_rnd(lua_State *L) {
    lua_Number n = luaL_checknumber(L, 1);
    int range = (int)n;
    if (range <= 0) range = 1;
    lua_pushinteger(L, rand() % range);
    return 1;
}

static int l_print(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    if (lua_gettop(L) >= 3) {
        int x = (int)luaL_checkinteger(L, 2);
        int y = (int)luaL_checkinteger(L, 3);
        printf("\033[%d;%dH%s", y + 1, x + 1, s);
    } else {
        printf("%s\n", s);
    }
    fflush(stdout);
    return 0;
}

static int l_stat(lua_State *L) {
    int k = (int)luaL_checkinteger(L, 1);
    if (k == 0) {
        lua_pushinteger(L, (lua_Integer)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    } else {
        lua_pushinteger(L, 0);
    }
    return 1;
}

static int l_scrsize(lua_State *L) {
    int rows, cols;
    vterm_get_size(&rows, &cols);
    lua_pushinteger(L, cols);
    lua_pushinteger(L, rows);
    return 2;
}

static int l_sleep(lua_State *L) {
    lua_Number ms = luaL_checknumber(L, 1);
    vTaskDelay((unsigned int)(ms / 10.0 + 0.5));
    return 0;
}

static const luaL_Reg g_pico_api[] = {
    { "cls",      l_cls      },
    { "mode",     l_mode     },
    { "pset",     l_pset     },
    { "line",     l_line     },
    { "rect",     l_rect     },
    { "rectfill", l_rectfill },
    { "spr",      l_spr      },
    { "pal",      l_pal      },
    { "flip",     l_flip     },
    { "btn",      l_btn      },
    { "time",     l_time     },
    { "rnd",      l_rnd      },
    { "print",    l_print    },
    { "stat",     l_stat     },
    { "scrsize",  l_scrsize  },
    { "sleep",    l_sleep    },
    { NULL, NULL }
};

static void register_pico_api(lua_State *L) {
    for (const luaL_Reg *r = g_pico_api; r->name; r++) {
        lua_register(L, r->name, r->func);
    }

    lua_pushinteger(L, 0x52); lua_setglobal(L, "KEY_UP");
    lua_pushinteger(L, 0x51); lua_setglobal(L, "KEY_DOWN");
    lua_pushinteger(L, 0x50); lua_setglobal(L, "KEY_LEFT");
    lua_pushinteger(L, 0x4F); lua_setglobal(L, "KEY_RIGHT");
    lua_pushinteger(L, 0x28); lua_setglobal(L, "KEY_ENTER");
    lua_pushinteger(L, 0x29); lua_setglobal(L, "KEY_ESC");
    lua_pushinteger(L, 0x2C); lua_setglobal(L, "KEY_SPACE");
    lua_pushinteger(L, 0x1D); lua_setglobal(L, "KEY_Z");
    lua_pushinteger(L, 0x1B); lua_setglobal(L, "KEY_X");
    lua_pushinteger(L, 0x04); lua_setglobal(L, "KEY_A");
    lua_pushinteger(L, 0x05); lua_setglobal(L, "KEY_B");
    lua_pushinteger(L, 0x3A); lua_setglobal(L, "KEY_F1");
    lua_pushinteger(L, 0x3B); lua_setglobal(L, "KEY_F2");
    lua_pushinteger(L, 0x3C); lua_setglobal(L, "KEY_F3");
    lua_pushinteger(L, 0x3D); lua_setglobal(L, "KEY_F4");

    lua_pushinteger(L, 3);    lua_setglobal(L, "SM_TEXT");
    lua_pushinteger(L, 0x13); lua_setglobal(L, "SM_VGA13H");
    lua_pushinteger(L, 0x80); lua_setglobal(L, "SM_150P");
}

/* ========== Simple REPL ========== */

static int repl_readline(char *buf, int maxlen) {
    int len = 0;
    while (len < maxlen - 1) {
        int c = getchar();
        if (c == EOF || c < 0) {
            vTaskDelay(1);
            continue;
        }
        if (c == 3 || c == 4) return -1;
        if (c == '\r' || c == '\n') {
            putchar('\n');
            fflush(stdout);
            break;
        }
        if (c == 127 || c == 8) {
            if (len > 0) {
                len--;
                putchar('\b'); putchar(' '); putchar('\b');
                fflush(stdout);
            }
            continue;
        }
        buf[len++] = (char)c;
        putchar(c);
        fflush(stdout);
    }
    buf[len] = '\0';
    return len;
}

static void run_repl(lua_State *L) {
    int rows, cols;
    vterm_get_size(&rows, &cols);

    printf("Lua 5.4 REPL (%dx%d)  Ctrl+C to exit\n", cols, rows);
    fflush(stdout);

    char line[256];

    while (1) {
        printf("> ");
        fflush(stdout);

        int len = repl_readline(line, sizeof(line));
        if (len < 0) break;

        if (strcmp(line, ":quit") == 0 || strcmp(line, "quit") == 0)
            break;
        if (line[0] == '\0')
            continue;

        char chunk[272];
        snprintf(chunk, sizeof(chunk), "return %s", line);
        int rc = luaL_dostring(L, chunk);
        if (rc != LUA_OK) {
            lua_pop(L, 1);
            rc = luaL_dostring(L, line);
        }
        if (rc != LUA_OK) {
            fprintf(stderr, "%s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else {
            int n = lua_gettop(L);
            for (int i = 1; i <= n; i++) {
                const char *v = lua_tostring(L, i);
                if (v) printf("%s%s", v, i < n ? "\t" : "\n");
            }
            if (n > 0) fflush(stdout);
            lua_settop(L, 0);
        }
    }
}

/* ========== Entry point ========== */

int app_main(int argc, char **argv) {
    lua_State *L = lua_newstate(lua_psram_alloc, NULL);
    if (!L) {
        fprintf(stderr, "lua_newstate failed\n");
        return 1;
    }

    luaL_openlibs(L);
    register_pico_api(L);

    int rc = 0;
    if (argc >= 2) {
        rc = luaL_dofile(L, argv[1]);
        if (rc != LUA_OK) {
            fprintf(stderr, "%s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        run_repl(L);
    }

    lua_close(L);
    return rc != LUA_OK ? 1 : 0;
}
