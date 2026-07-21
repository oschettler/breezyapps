/*
 * edit.c - nano-style text editor for BreezyBox
 *
 * Key bindings:
 *   Arrow keys    Move cursor
 *   Page Up/Down  Scroll
 *   Home / End    Start / end of line
 *   Ctrl+S        Save
 *   Ctrl+X        Exit (prompts if modified)
 *   Ctrl+R        Save + run via breezybox_exec("lua <file>")
 *   Ctrl+K        Cut line
 *   Ctrl+U        Paste cut line
 *   Backspace/Del Delete character
 *   Enter         Insert newline
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>

/* ========== Configuration ========== */
#define MAX_LINES       8192
#define MAX_LINE_LEN    4096
#define ESC_BUF_SIZE    8

/* ========== ANSI Escape Codes ========== */
#define ESC_CLEAR       "\033[2J"
#define ESC_HOME        "\033[H"
#define ESC_CURSOR_HIDE "\033[?25l"
#define ESC_CURSOR_SHOW "\033[?25h"
#define ESC_RESET       "\033[0m"
#define ESC_REVERSE     "\033[7m"
#define ESC_BOLD        "\033[1m"
#define ESC_CYAN        "\033[36m"

/* ========== Platform abstraction ========== */
#if defined(__XTENSA__) || defined(__riscv)

void vterm_get_size(int *rows, int *cols);
int  breezybox_exec(const char *cmdline);

static int s_orig_fcntl;

static void plat_init(void) {
    s_orig_fcntl = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, s_orig_fcntl | O_NONBLOCK);
}

static void plat_cleanup(void) {
    fcntl(STDIN_FILENO, F_SETFL, s_orig_fcntl);
}

static void plat_get_size(int *rows, int *cols) {
    vterm_get_size(rows, cols);
}

static void plat_delay_ms(int ms) {
    extern void vTaskDelay(unsigned int);
    vTaskDelay(ms / 10);
}

static int plat_exec(const char *cmdline) {
    plat_cleanup();
    int rc = breezybox_exec(cmdline);
    plat_init();
    return rc;
}

#else

#include <sys/ioctl.h>
#include <termios.h>

static struct termios s_orig_termios;

static void plat_init(void) {
    tcgetattr(STDIN_FILENO, &s_orig_termios);
    struct termios raw = s_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void plat_cleanup(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &s_orig_termios);
}

static void plat_get_size(int *rows, int *cols) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        *rows = w.ws_row;
        *cols = w.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

static void plat_delay_ms(int ms) {
    usleep(ms * 1000);
}

static int plat_exec(const char *cmdline) {
    plat_cleanup();
    int rc = system(cmdline);
    printf("\n[Press any key to continue]");
    fflush(stdout);
    getchar();
    plat_init();
    return rc;
}

#endif

/* ========== Special Keys ========== */
enum {
    KEY_NONE = 0,
    KEY_UP = 1000,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_DELETE,
    KEY_BACKSPACE,
    KEY_ENTER,
    KEY_ESC,
    KEY_CTRL_S,
    KEY_CTRL_X,
    KEY_CTRL_R,
    KEY_CTRL_K,
    KEY_CTRL_U,
    KEY_CTRL_C,
};

/* ========== Editor state ========== */
static struct {
    char **lines;
    int num_lines;
    int lines_alloc;

    int cur_row, cur_col;
    int top_row;
    int left_col;

    int modified;
    int running;
    int exit_confirmed;

    char filepath[256];
    char status[256];
    char *cut_line;

    int screen_rows, screen_cols;

    /* Redraw tracking: avoid repainting all visible rows on every keystroke.
       dirty_row is the single file row whose content changed (-1 = none);
       force_full_redraw is set whenever line count changes or the screen
       was cleared out-of-band (e.g. running a script). Pure cursor motion
       and same-row edits then only touch one line instead of the whole page. */
    int dirty_row;
    int force_full_redraw;
} E;

/* ========== Output buffering ========== */
static char s_out[32768];
static int  s_out_pos;

static void out_flush(void) {
    if (s_out_pos > 0) {
        write(STDOUT_FILENO, s_out, s_out_pos);
        s_out_pos = 0;
    }
}

static void out_str(const char *s) {
    int len = strlen(s);
    if (s_out_pos + len >= (int)sizeof(s_out)) out_flush();
    if (len < (int)sizeof(s_out)) {
        memcpy(&s_out[s_out_pos], s, len);
        s_out_pos += len;
    } else {
        out_flush();
        write(STDOUT_FILENO, s, len);
    }
}

static void out_char(char c) {
    if (s_out_pos >= (int)sizeof(s_out) - 1) out_flush();
    s_out[s_out_pos++] = c;
}

static void out_int(int n) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", n);
    out_str(buf);
}

static void out_goto(int row, int col) {
    out_str("\033[");
    out_int(row + 1);
    out_char(';');
    out_int(col + 1);
    out_char('H');
}

/* ========== Line management ========== */
static char *line_dup(const char *s) {
    if (!s) s = "";
    size_t len = strlen(s);
    char *p = malloc(len + 1);
    if (p) memcpy(p, s, len + 1);
    return p;
}

static void ensure_capacity(int needed) {
    if (needed <= E.lines_alloc) return;
    int na = E.lines_alloc ? E.lines_alloc * 2 : 64;
    while (na < needed) na *= 2;
    E.lines = realloc(E.lines, na * sizeof(char *));
    E.lines_alloc = na;
}

static void insert_line_at(int idx, const char *text) {
    ensure_capacity(E.num_lines + 1);
    for (int i = E.num_lines; i > idx; i--)
        E.lines[i] = E.lines[i - 1];
    E.lines[idx] = line_dup(text);
    E.num_lines++;
}

static void delete_line_at(int idx) {
    if (idx < 0 || idx >= E.num_lines) return;
    free(E.lines[idx]);
    for (int i = idx; i < E.num_lines - 1; i++)
        E.lines[i] = E.lines[i + 1];
    E.num_lines--;
    if (E.num_lines == 0) insert_line_at(0, "");
}

static int line_len(int row) {
    if (row < 0 || row >= E.num_lines) return 0;
    return strlen(E.lines[row]);
}

/* ========== Lua keyword detection (for mild syntax coloring) ========== */
static const struct { const char *word; int len; } s_lua_keywords[] = {
    { "if",       2 }, { "then",     4 }, { "else",     4 }, { "elseif",   6 },
    { "end",      3 }, { "for",      3 }, { "while",    5 }, { "do",       2 },
    { "function", 8 }, { "return",   6 }, { "local",    5 }, { "and",      3 },
    { "or",       2 }, { "not",      3 }, { "nil",      3 }, { "true",     4 },
    { "false",    5 }, { "repeat",   6 }, { "until",    5 }, { "break",    5 },
    { "in",       2 }, { NULL,       0 }
};

static int is_lua_keyword(const char *s, int len) {
    for (int i = 0; s_lua_keywords[i].word; i++) {
        if (s_lua_keywords[i].len == len &&
            memcmp(s, s_lua_keywords[i].word, len) == 0)
            return 1;
    }
    return 0;
}

/* ========== File I/O ========== */
static void load_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        insert_line_at(0, "");
        snprintf(E.status, sizeof(E.status), "[New File] %s", path);
        return;
    }
    char buf[MAX_LINE_LEN];
    while (fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
            buf[--len] = '\0';
        insert_line_at(E.num_lines, buf);
    }
    fclose(f);
    if (E.num_lines == 0) insert_line_at(0, "");
    snprintf(E.status, sizeof(E.status), "\"%s\" %d lines", path, E.num_lines);
}

static int save_file(void) {
    if (!E.filepath[0]) {
        snprintf(E.status, sizeof(E.status), "No filename");
        return 0;
    }
    FILE *f = fopen(E.filepath, "w");
    if (!f) {
        snprintf(E.status, sizeof(E.status), "Cannot write: %s", E.filepath);
        return 0;
    }
    for (int i = 0; i < E.num_lines; i++)
        fprintf(f, "%s\n", E.lines[i]);
    fclose(f);
    E.modified = 0;
    snprintf(E.status, sizeof(E.status), "\"%s\" written (%d lines)", E.filepath, E.num_lines);
    return 1;
}

/* ========== Input ========== */
static int read_key(void) {
    static char esc[ESC_BUF_SIZE];
    static int  esc_len = 0;

    char c;
    int n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return KEY_NONE;

    if (esc_len > 0 || c == 27) {
        esc[esc_len++] = c;
        if (esc_len >= 3) {
            if (memcmp(esc, "\033[A", 3) == 0) { esc_len = 0; return KEY_UP; }
            if (memcmp(esc, "\033[B", 3) == 0) { esc_len = 0; return KEY_DOWN; }
            if (memcmp(esc, "\033[C", 3) == 0) { esc_len = 0; return KEY_RIGHT; }
            if (memcmp(esc, "\033[D", 3) == 0) { esc_len = 0; return KEY_LEFT; }
            if (memcmp(esc, "\033[H", 3) == 0) { esc_len = 0; return KEY_HOME; }
            if (memcmp(esc, "\033[F", 3) == 0) { esc_len = 0; return KEY_END; }
            if (memcmp(esc, "\033OH", 3) == 0) { esc_len = 0; return KEY_HOME; }
            if (memcmp(esc, "\033OF", 3) == 0) { esc_len = 0; return KEY_END; }
        }
        if (esc_len >= 4) {
            if (memcmp(esc, "\033[3~", 4) == 0) { esc_len = 0; return KEY_DELETE; }
            if (memcmp(esc, "\033[1~", 4) == 0) { esc_len = 0; return KEY_HOME; }
            if (memcmp(esc, "\033[4~", 4) == 0) { esc_len = 0; return KEY_END; }
            if (memcmp(esc, "\033[5~", 4) == 0) { esc_len = 0; return KEY_PAGE_UP; }
            if (memcmp(esc, "\033[6~", 4) == 0) { esc_len = 0; return KEY_PAGE_DOWN; }
        }
        if (esc_len == 1) {
            char c2;
            if (read(STDIN_FILENO, &c2, 1) <= 0) { esc_len = 0; return KEY_ESC; }
            esc[esc_len++] = c2;
        }
        if (esc_len >= ESC_BUF_SIZE - 1) esc_len = 0;
        return KEY_NONE;
    }

    if (c == 127 || c == 8)  return KEY_BACKSPACE;
    if (c == '\r' || c == '\n') return KEY_ENTER;
    if (c == 19) return KEY_CTRL_S;
    if (c == 24) return KEY_CTRL_X;
    if (c == 18) return KEY_CTRL_R;
    if (c == 11) return KEY_CTRL_K;
    if (c == 21) return KEY_CTRL_U;
    if (c ==  3) return KEY_CTRL_C;
    if (c >= 32 && c < 127) return (unsigned char)c;
    return KEY_NONE;
}

/* ========== Viewport / cursor ========== */
static void clamp_cursor(void) {
    if (E.cur_row < 0) E.cur_row = 0;
    if (E.cur_row >= E.num_lines) E.cur_row = E.num_lines - 1;
    if (E.cur_row < 0) E.cur_row = 0;
    int len = line_len(E.cur_row);
    if (E.cur_col > len) E.cur_col = len;
    if (E.cur_col < 0)   E.cur_col = 0;
}

static void adjust_viewport(void) {
    int text_rows = E.screen_rows - 2;
    if (E.cur_row < E.top_row) E.top_row = E.cur_row;
    if (E.cur_row >= E.top_row + text_rows)
        E.top_row = E.cur_row - text_rows + 1;
    if (E.cur_col < E.left_col) E.left_col = E.cur_col;
    if (E.cur_col >= E.left_col + E.screen_cols)
        E.left_col = E.cur_col - E.screen_cols + 1;
}

/* ========== Rendering ========== */
static void draw_line(int file_row, int scr_y) {
    out_goto(scr_y, 0);
    if (file_row >= E.num_lines) {
        out_char('~');
        for (int i = 1; i < E.screen_cols; i++) out_char(' ');
        return;
    }

    const char *line = E.lines[file_row];
    int len = strlen(line);
    int start = E.left_col;
    int end_col = start + E.screen_cols;

    int col = start;
    int in_string = 0;
    char str_char = 0;
    int in_comment = 0;

    while (col < end_col) {
        if (col >= len) {
            out_char(' ');
            col++;
            continue;
        }
        char ch = line[col];

        if (!in_string && !in_comment && col + 1 < len &&
            line[col] == '-' && line[col + 1] == '-') {
            out_str(ESC_CYAN);
            in_comment = 1;
        }

        if (!in_comment) {
            if (!in_string && (ch == '"' || ch == '\'')) {
                in_string = 1;
                str_char = ch;
                out_str(ESC_CYAN);
            } else if (in_string && ch == str_char &&
                       (col == 0 || line[col-1] != '\\')) {
                out_char(ch);
                out_str(ESC_RESET);
                col++;
                in_string = 0;
                continue;
            }
        }

        if (!in_string && !in_comment && (isalpha((unsigned char)ch) || ch == '_')) {
            int kstart = col;
            while (col < len && (isalnum((unsigned char)line[col]) || line[col] == '_'))
                col++;
            int klen = col - kstart;
            if (is_lua_keyword(line + kstart, klen)) {
                out_str(ESC_BOLD);
                for (int k = kstart; k < col && k < end_col; k++)
                    out_char(line[k]);
                out_str(ESC_RESET);
            } else {
                for (int k = kstart; k < col && k < end_col; k++)
                    out_char(line[k]);
            }
            continue;
        }

        out_char(ch);
        col++;
    }

    if (in_string || in_comment) out_str(ESC_RESET);
}

static void draw_screen(void) {
    int old_top = E.top_row, old_left = E.left_col;
    adjust_viewport();
    int scrolled = (E.top_row != old_top || E.left_col != old_left);
    int full = E.force_full_redraw || scrolled;

    out_str(ESC_CURSOR_HIDE);

    int text_rows = E.screen_rows - 2;

    if (full) {
        for (int y = 0; y < text_rows; y++) {
            draw_line(E.top_row + y, y);
        }
    } else if (E.dirty_row >= 0) {
        int y = E.dirty_row - E.top_row;
        if (y >= 0 && y < text_rows) draw_line(E.dirty_row, y);
    }
    E.dirty_row = -1;
    E.force_full_redraw = 0;

    /* Top status bar */
    out_goto(E.screen_rows - 2, 0);
    out_str(ESC_REVERSE);
    {
        char left[128], right[64];
        snprintf(left, sizeof(left), " %s%s",
                 E.filepath[0] ? E.filepath : "[No Name]",
                 E.modified ? " [+]" : "");
        snprintf(right, sizeof(right), "Ln %d Col %d ",
                 E.cur_row + 1, E.cur_col + 1);
        int llen = strlen(left);
        int rlen = strlen(right);
        int w = E.screen_cols - 1;
        int pad = w - llen - rlen;
        if (pad < 0) pad = 0;
        int pos = 0;
        for (int i = 0; i < llen && pos < w; i++, pos++) out_char(left[i]);
        for (int i = 0; i < pad && pos < w; i++, pos++) out_char(' ');
        for (int i = 0; i < rlen && pos < w; i++, pos++) out_char(right[i]);
    }
    out_str(ESC_RESET);

    /* Bottom help bar */
    out_goto(E.screen_rows - 1, 0);
    out_str(ESC_REVERSE);
    {
        const char *help = " ^S Save  ^X Exit  ^R Run  ^K Cut  ^U Paste";
        if (E.status[0]) help = E.status;
        int hlen = strlen(help);
        int w = E.screen_cols - 1;
        for (int i = 0; i < hlen && i < w; i++) out_char(help[i]);
        for (int i = hlen; i < w; i++) out_char(' ');
        E.status[0] = '\0';
    }
    out_str(ESC_RESET);

    /* Cursor */
    int sr = E.cur_row - E.top_row;
    int sc = E.cur_col - E.left_col;
    out_goto(sr, sc);
    out_str(ESC_CURSOR_SHOW);
    out_flush();
}

/* ========== Editing operations ========== */
static void insert_char(char c) {
    char *line = E.lines[E.cur_row];
    int len = strlen(line);
    char *nl = malloc(len + 2);
    memcpy(nl, line, E.cur_col);
    nl[E.cur_col] = c;
    memcpy(nl + E.cur_col + 1, line + E.cur_col, len - E.cur_col + 1);
    free(E.lines[E.cur_row]);
    E.lines[E.cur_row] = nl;
    E.cur_col++;
    E.modified = 1;
    E.dirty_row = E.cur_row;
}

static void delete_char_at(int col) {
    char *line = E.lines[E.cur_row];
    int len = strlen(line);
    if (col < 0 || col >= len) return;
    memmove(line + col, line + col + 1, len - col);
    E.modified = 1;
    E.dirty_row = E.cur_row;
}

static void backspace_char(void) {
    if (E.cur_col > 0) {
        E.cur_col--;
        delete_char_at(E.cur_col);
    } else if (E.cur_row > 0) {
        int prev_len = line_len(E.cur_row - 1);
        char *prev = E.lines[E.cur_row - 1];
        char *curr = E.lines[E.cur_row];
        char *nl = malloc(prev_len + strlen(curr) + 1);
        strcpy(nl, prev);
        strcat(nl, curr);
        free(E.lines[E.cur_row - 1]);
        E.lines[E.cur_row - 1] = nl;
        delete_line_at(E.cur_row);
        E.cur_row--;
        E.cur_col = prev_len;
        E.modified = 1;
        E.force_full_redraw = 1;
    }
}

static void insert_newline(void) {
    char *line = E.lines[E.cur_row];
    char *rest = line_dup(line + E.cur_col);
    line[E.cur_col] = '\0';
    insert_line_at(E.cur_row + 1, rest);
    free(rest);
    E.cur_row++;
    E.cur_col = 0;
    E.modified = 1;
    E.force_full_redraw = 1;
}

static void cut_line(void) {
    if (E.cut_line) free(E.cut_line);
    E.cut_line = line_dup(E.lines[E.cur_row]);
    delete_line_at(E.cur_row);
    clamp_cursor();
    E.modified = 1;
    E.force_full_redraw = 1;
    snprintf(E.status, sizeof(E.status), "Line cut");
}

static void paste_line(void) {
    if (!E.cut_line) return;
    insert_line_at(E.cur_row, E.cut_line);
    E.modified = 1;
    E.force_full_redraw = 1;
    snprintf(E.status, sizeof(E.status), "Line pasted");
}

/* ========== Ctrl+R: save + run ========== */
static void run_file(void) {
    if (!E.filepath[0]) {
        snprintf(E.status, sizeof(E.status), "No filename to run");
        return;
    }
    if (!save_file()) return;

    /* Restore terminal before handing off */
    out_str(ESC_CLEAR);
    out_str(ESC_HOME);
    out_str(ESC_CURSOR_SHOW);
    out_str(ESC_RESET);
    out_flush();

    char cmd[272];
    snprintf(cmd, sizeof(cmd), "lua \"%s\"", E.filepath);
    int rc = plat_exec(cmd);

    /* Lua returned — redraw editor */
    out_str(ESC_CLEAR);
    out_str(ESC_HOME);
    out_flush();
    E.force_full_redraw = 1;
    if (rc != 0) {
        snprintf(E.status, sizeof(E.status), "Run failed (rc=%d): %s", rc, E.filepath);
    } else {
        snprintf(E.status, sizeof(E.status), "Ran: %s", E.filepath);
    }
}

/* ========== Exit with prompt ========== */
static void try_exit(void) {
    if (!E.modified) {
        E.running = 0;
        return;
    }
    if (E.exit_confirmed) {
        E.running = 0;
        return;
    }
    snprintf(E.status, sizeof(E.status),
             "File modified. Press ^X again to exit without saving, or ^S to save");
    E.exit_confirmed = 1;
}

/* ========== Main key handler ========== */
static void handle_key(int key) {
    if (key != KEY_CTRL_X) E.exit_confirmed = 0;

    switch (key) {
        case KEY_UP:
            if (E.cur_row > 0) E.cur_row--;
            clamp_cursor();
            break;
        case KEY_DOWN:
            if (E.cur_row < E.num_lines - 1) E.cur_row++;
            clamp_cursor();
            break;
        case KEY_LEFT:
            if (E.cur_col > 0) {
                E.cur_col--;
            } else if (E.cur_row > 0) {
                E.cur_row--;
                E.cur_col = line_len(E.cur_row);
            }
            break;
        case KEY_RIGHT:
            if (E.cur_col < line_len(E.cur_row)) {
                E.cur_col++;
            } else if (E.cur_row < E.num_lines - 1) {
                E.cur_row++;
                E.cur_col = 0;
            }
            break;
        case KEY_HOME:
            E.cur_col = 0;
            break;
        case KEY_END:
            E.cur_col = line_len(E.cur_row);
            break;
        case KEY_PAGE_UP: {
            int page = E.screen_rows - 2;
            E.cur_row -= page;
            E.top_row -= page;
            if (E.top_row < 0) E.top_row = 0;
            clamp_cursor();
            break;
        }
case KEY_PAGE_DOWN: {
            int page = E.screen_rows - 2;
            E.cur_row += page;
            E.top_row += page;
            clamp_cursor();
            if (E.top_row > E.cur_row) E.top_row = E.cur_row;
            break;
        }
        case KEY_BACKSPACE:
            backspace_char();
            break;
        case KEY_DELETE:
            if (E.cur_col < line_len(E.cur_row)) {
                delete_char_at(E.cur_col);
            } else if (E.cur_row < E.num_lines - 1) {
                char *curr = E.lines[E.cur_row];
                char *next = E.lines[E.cur_row + 1];
                char *nl = malloc(strlen(curr) + strlen(next) + 1);
                strcpy(nl, curr);
                strcat(nl, next);
                free(E.lines[E.cur_row]);
                E.lines[E.cur_row] = nl;
                delete_line_at(E.cur_row + 1);
                E.modified = 1;
                E.force_full_redraw = 1;
            }
            break;
        case KEY_ENTER:
            insert_newline();
            break;
        case KEY_CTRL_S:
            save_file();
            break;
        case KEY_CTRL_X:
            try_exit();
            break;
        case KEY_CTRL_R:
            run_file();
            break;
        case KEY_CTRL_K:
            cut_line();
            break;
        case KEY_CTRL_U:
            paste_line();
            break;
        case KEY_CTRL_C:
            E.running = 0;
            break;
        default:
            if (key >= 32 && key < 127) {
                insert_char((char)key);
            }
            break;
    }
}

/* ========== Entry point ========== */
int app_main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: edit <filename>\n");
        return 1;
    }

    memset(&E, 0, sizeof(E));
    E.running = 1;
    E.dirty_row = -1;
    E.force_full_redraw = 1;
    strncpy(E.filepath, argv[1], sizeof(E.filepath) - 1);

    plat_init();
    plat_get_size(&E.screen_rows, &E.screen_cols);

    load_file(argv[1]);

    out_str(ESC_CLEAR);
    out_str(ESC_HOME);
    out_flush();

    draw_screen();

    while (E.running) {
        int key = read_key();
        if (key == KEY_NONE) {
            plat_delay_ms(10);
            continue;
        }
        handle_key(key);
        draw_screen();
    }

    out_str(ESC_CLEAR);
    out_str(ESC_HOME);
    out_str(ESC_CURSOR_SHOW);
    out_str(ESC_RESET);
    out_flush();

    if (E.cut_line) free(E.cut_line);
    for (int i = 0; i < E.num_lines; i++) free(E.lines[i]);
    free(E.lines);

    plat_cleanup();
    return 0;
}
