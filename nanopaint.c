/*
 * nanopaint
 *
 * Terminal ASCII paint brush
 *
 * Build: clang -O3 -Wall -Wextra -o nanopaint nanopaint.c
 * Run:   ./nanopaint [filename]
 */

#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

/* ----------------------------------------------------------------- */
/*  Constants                                                         */
/* ----------------------------------------------------------------- */

#define CSI             "\x1b["          /* Control Sequence Introducer */
#define ESC             "\x1b"           /* Escape character */

/* Terminal mode strings (used with write()) */
#define MODE_ALT_BUF    CSI "?1049h" CSI "?25l" CSI "?1003h" CSI "?1006h"
#define MODE_NORMAL     CSI "?25h"   CSI "?1003l" CSI "?1006l" CSI "?1049l"
#define CLR_SCREEN      CSI "2J"
#define HOME_CURSOR     CSI "H"

/* printf()-style format strings */
#define FMT_CURSOR_ROW  CSI "%d;1H"      /* move to start of row N */
#define FMT_CURSOR_POS  CSI "%d;%dH"     /* move to (row, col) */
#define FMT_SET_COLOR   CSI "%dm"        /* set foreground colour */
#define RESET_COLOR     CSI "39m"        /* reset foreground colour */
#define REV_ON          CSI "7m"         /* reverse video on */
#define REV_OFF         CSI "27m"        /* reverse video off */
#define CLR_EOL         CSI "K"          /* clear to end of line */

/* Key / control codes */
#define KEY_ESC         27
#define KEY_BACKSPACE   127
#define KEY_CTRL_C      3
#define KEY_CTRL_S      19
#define KEY_CTRL_T      20
#define KEY_CTRL_Y      25
#define KEY_CTRL_Z      26

/* Canvas cell — each cell holds a UTF-8 character (up to 4 bytes + NUL) */
#define CELL_SIZE       5

/* Default colour index used for blank cells and the initial brush */
#define DEFAULT_COLOR   7

/* Printable ASCII range (space through tilde) */
#define ASCII_MIN       32
#define ASCII_MAX       126

/* ----------------------------------------------------------------- */
/*  Terminal state                                                   */
/* ----------------------------------------------------------------- */

static struct termios orig_tios;       /* saved terminal attributes */
static volatile sig_atomic_t quit;     /* exit flag for signal handler */

/* ----------------------------------------------------------------- */
/*  File path for saving                                              */
/* ----------------------------------------------------------------- */

static const char *filepath = NULL;

/* ----------------------------------------------------------------- */
/*  Canvas — a 2D grid of characters (cell = char[CELL_SIZE])        */
/* ----------------------------------------------------------------- */
/*  Each cell holds a single character, up to 4 bytes (UTF-8) plus   */
/*  a null terminator. This means emojis are supported.               */
/* ----------------------------------------------------------------- */

static char (*canvas)[CELL_SIZE] = NULL; /* the cell grid */
static int canvas_rows = 0;            /* number of rows in canvas */
static int canvas_cols = 0;            /* number of columns in canvas */

/* ----------------------------------------------------------------- */
/*  Cursor position — where the mouse pointer is on screen           */
/* ----------------------------------------------------------------- */

static int mx = -1, my = -1;          /* 1-based, -1 = hasn't moved yet */

/* ----------------------------------------------------------------- */
/*  Brush — the character and colour painted on click-drag            */
/* ----------------------------------------------------------------- */

static char brush[CELL_SIZE] = "#";
static int brush_color = DEFAULT_COLOR; /* 0-9, DEFAULT_COLOR = white */

/* ----------------------------------------------------------------- */
/*  Drawing state                                                     */
/* ----------------------------------------------------------------- */

static int is_drawing = 0;
static int last_paint_x = -1, last_paint_y = -1;

/* ----------------------------------------------------------------- */
/*  Text typing mode                                                  */
/* ----------------------------------------------------------------- */

static int typing_mode = 0;              /* 1 = typing active */
static int typing_x = -1, typing_y = -1; /* cursor position, -1 = unset */
static int typing_anchor_x = -1, typing_anchor_y = -1; /* where click landed */
static int mouse_seen = 0;               /* set by handleMouseEvent on valid parse */

#define MAX_TYPING_HISTORY 4096

struct TypingEntry {
    int x, y;
    char old_char[CELL_SIZE];
    unsigned char old_color;
    int is_newline;
};

static struct TypingEntry typing_history[MAX_TYPING_HISTORY];
static int typing_history_count = 0;

/* ----------------------------------------------------------------- */
/*  Cell colours — parallel array, one byte per cell, 0-9             */
/* ----------------------------------------------------------------- */

static unsigned char *cell_colors = NULL;

/* ----------------------------------------------------------------- */
/*  ANSI colour codes for colour indices 0-9                          */
/* ----------------------------------------------------------------- */

static const int ansi_colors[] = {
    30, 31, 32, 33, 34, 35, 36, 37, 90, 91
    // 0=black 1=red 2=grn 3=yel 4=blu 5=mag 6=cyn 7=wht 8=brblk 9=brred
};

static const char *color_names[] = {
    "black", "red", "green", "yellow", "blue",
    "magenta", "cyan", "white", "brblk", "brred"
};

/* ----------------------------------------------------------------- */
/*  Undo / Redo                                                       */
/* ----------------------------------------------------------------- */
/*  Each stroke saves the canvas state so we can undo/redo.           */
/*  MAX_UNDO limits memory; older states are dropped.                 */
/* ----------------------------------------------------------------- */

#define MAX_UNDO 100

struct CanvasState {
    char *canvas;
    unsigned char *colors;
    int rows;
    int cols;
};

static struct CanvasState *undo_stack[MAX_UNDO];
static int undo_count = 0;
static struct CanvasState *redo_stack[MAX_UNDO];
static int redo_count = 0;

static struct CanvasState *save_state(void) {
    struct CanvasState *s = malloc(sizeof(*s));
    int n = canvas_rows * canvas_cols;
    s->rows = canvas_rows;
    s->cols = canvas_cols;
    s->canvas = malloc(n * CELL_SIZE);
    s->colors = malloc(n);
    if (s->canvas) memcpy(s->canvas, canvas, n * CELL_SIZE);
    if (s->colors) memcpy(s->colors, cell_colors, n);
    return s;
}

static void restore_state(struct CanvasState *s) {
    int n = canvas_rows * canvas_cols;
    memcpy(canvas, s->canvas, n * CELL_SIZE);
    memcpy(cell_colors, s->colors, n);
}

static void free_state(struct CanvasState *s) {
    free(s->canvas);
    free(s->colors);
    free(s);
}

// Call BEFORE modifying the canvas.
static void push_undo(void) {
    if (undo_count >= MAX_UNDO) {
        free_state(undo_stack[0]);
        memmove(undo_stack, undo_stack + 1, (MAX_UNDO - 1) * sizeof(void *));
        undo_count = MAX_UNDO - 1;
    }
    undo_stack[undo_count++] = save_state();
    for (int i = 0; i < redo_count; i++) free_state(redo_stack[i]);
    redo_count = 0;
}

static void undo(void) {
    if (undo_count == 0) return;
    struct CanvasState *s = undo_stack[--undo_count];
    if (redo_count >= MAX_UNDO) {
        free_state(redo_stack[0]);
        memmove(redo_stack, redo_stack + 1, (MAX_UNDO - 1) * sizeof(void *));
        redo_count = MAX_UNDO - 1;
    }
    redo_stack[redo_count++] = save_state();
    restore_state(s);
    free_state(s);
}

static void redo(void) {
    if (redo_count == 0) return;
    struct CanvasState *s = redo_stack[--redo_count];
    if (undo_count >= MAX_UNDO) {
        free_state(undo_stack[0]);
        memmove(undo_stack, undo_stack + 1, (MAX_UNDO - 1) * sizeof(void *));
        undo_count = MAX_UNDO - 1;
    }
    undo_stack[undo_count++] = save_state();
    restore_state(s);
    free_state(s);
}

/* ----------------------------------------------------------------- */
/*  Command table                                                     */
/* ----------------------------------------------------------------- */

struct Command {
    unsigned char key;
    const char *label;    // Short label for toolbar
    const char *desc;     // Description
    void (*handler)(void);
};

/* ----------------------------------------------------------------- */
/*  UTF-8 helpers                                                     */
/* ----------------------------------------------------------------- */

static int utf8_len(unsigned char c) {
    if (c >= 0xF0) return 4;   /* 4-byte character */
    if (c >= 0xE0) return 3;   /* 3-byte character */
    if (c >= 0xC0) return 2;   /* 2-byte character */
    return 1;                   /* 1-byte ASCII */
}

// Copy a UTF-8 character from src to dst (up to 4 bytes + null).
static void utf8_cpy(char *dst, const char *src) {
    int len = utf8_len((unsigned char)*src);
    int i;
    for (i = 0; i < len; i++) dst[i] = src[i];
    dst[len] = '\0';
}

// Advance s to the next UTF-8 character boundary.
static const char *utf8_next(const char *s) {
    return s + utf8_len((unsigned char)*s);
}

// Validate that N continuation bytes are legal UTF-8 tail bytes (each
// must match the pattern 10xxxxxx).
static int valid_utf8_tail(const unsigned char *tail, int n) {
    for (int i = 0; i < n; i++)
        if ((tail[i] & 0xC0) != 0x80) return 0;
    return 1;
}

/* ----------------------------------------------------------------- */
/*  Switch terminal to raw mode                                       */
/* ----------------------------------------------------------------- */

static void enableRawMode(void) {
    tcgetattr(STDIN_FILENO, &orig_tios);
    struct termios raw = orig_tios;

    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

/* ----------------------------------------------------------------- */
/*  Restore terminal to its original state                            */
/* ----------------------------------------------------------------- */

static void disableRawMode(void) {
    write(STDOUT_FILENO, MODE_NORMAL, sizeof(MODE_NORMAL) - 1);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_tios);
}

/* ----------------------------------------------------------------- */
/*  Signal handler — set quit flag on SIGINT/SIGTERM, restore         */
/*  terminal on crash signals (SIGSEGV, SIGBUS, SIGABRT).             */
/* ----------------------------------------------------------------- */

static void handler(int sig) {
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGABRT) {
        disableRawMode();
        _exit(1);
    }
    quit = 1;
}

/* ----------------------------------------------------------------- */
/*  Initialise the terminal for our program                           */
/* ----------------------------------------------------------------- */

static void initTerminal(void) {
    // Enter alternate buffer, hide cursor, enable mouse tracking & SGR
    // mode, then clear the screen and home the cursor.
    write(STDOUT_FILENO, MODE_ALT_BUF CLR_SCREEN HOME_CURSOR,
          sizeof(MODE_ALT_BUF CLR_SCREEN HOME_CURSOR) - 1);
}

/* ----------------------------------------------------------------- */
/*  Paint a single cell at the given 0-based canvas coordinates.      */
/*  The terminal doesn't report every pixel when the mouse moves      */
/*  fast, so we just paint wherever the mouse is reported to be.      */
/* ----------------------------------------------------------------- */

static void paint_cell(int x, int y) {
    if (!canvas || !cell_colors) return;
    if (x >= 0 && x < canvas_cols && y >= 0 && y < canvas_rows) {
        int idx = y * canvas_cols + x;
        strcpy(canvas[idx], brush);
        cell_colors[idx] = brush_color;
    }
}

// Bresenham's line algorithm — paint all cells between (x0,y0) and (x1,y1)
static void paint_line(int x0, int y0, int x1, int y1) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sy = y0 < y1 ? 1 : -1;
    int err = (dx > dy ? dx : -dy) / 2;
    for (;;) {
        paint_cell(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 <  dy) { err += dx; y0 += sy; }
    }
}

/* ----------------------------------------------------------------- */
/*  Create a blank canvas the size of the terminal                     */
/*  Used when no file is given on the command line.                    */
/* ----------------------------------------------------------------- */

static void initBlankCanvas(void) {
    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    canvas_rows = ws.ws_row;
    canvas_cols = ws.ws_col;
    canvas = malloc(canvas_rows * canvas_cols * CELL_SIZE);
    cell_colors = malloc(canvas_rows * canvas_cols);
    for (int i = 0; i < canvas_rows * canvas_cols; i++) {
        canvas[i][0] = ' ';
        canvas[i][1] = '\0';
        cell_colors[i] = DEFAULT_COLOR;
    }
}

/* ----------------------------------------------------------------- */
/*  Count visible UTF-8 characters in a line, skipping ANSI codes.    */
/* ----------------------------------------------------------------- */

// Count the visible UTF-8 characters in a line, skipping any ANSI
// escape sequences that might be embedded (e.g. colour codes from
// a previous save). Plain text with no ANSI codes works too.
static int count_visible_chars(const char *s) {
    int n = 0;
    while (*s) {
        if ((unsigned char)*s == KEY_ESC && *(s + 1) == '[') {
            s += 2;
            while (*s && *s != 'm') s++;
            if (*s == 'm') s++;
            continue;
        }
        s = utf8_next(s);
        n++;
    }
    return n;
}

// Fill one canvas row by parsing a line that may contain ANSI colour
// codes.  Each ESC[<N>m sequence sets the colour index for all
// following characters until the next code (or end of line).  Plain
// text with no ANSI codes gets colour 7 (white).
static void fill_row(const char *line, int row) {
    int c = 0, color = DEFAULT_COLOR;
    while (*line && c < canvas_cols) {
        if ((unsigned char)*line == KEY_ESC && *(line + 1) == '[') {
            line += 2;
            int val = 0;
            while (*line && *line != 'm') {
                if (*line >= '0' && *line <= '9') val = val * 10 + (*line - '0');
                line++;
            }
            if (*line == 'm') line++;
            if (val == 0 || val == 39) { color = DEFAULT_COLOR; }
            else {
                for (int i = 0; i < 10; i++) {
                    if (ansi_colors[i] == val) { color = i; break; }
                }
            }
            continue;
        }
        int idx = row * canvas_cols + c;
        utf8_cpy(canvas[idx], line);
        cell_colors[idx] = color;
        line = utf8_next(line);
        c++;
    }
}

/* ----------------------------------------------------------------- */
/*  Load a text file into the canvas (supports ANSI colour codes).    */
/* ----------------------------------------------------------------- */

static void loadFile(const char *path) {
    // ---- First pass: read all lines into a temporary array -----------
    //            and determine the canvas dimensions needed.
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char **tmp = NULL;       // temporary array of raw lines
    int tmpnum = 0;          // number of lines
    int tmpcap = 0;          // capacity of tmp array
    int max_line_chars = 0;  // max visible chars in any line
    char line[4096];

    while (fgets(line, sizeof(line), fp)) {
        // Strip the trailing newline
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

        // Grow the temporary array as needed
        if (tmpnum >= tmpcap) {
            tmpcap = tmpcap ? tmpcap * 2 : 256;
            tmp = realloc(tmp, tmpcap * sizeof(char *));
        }

        tmp[tmpnum] = strdup(line);

        // Update the maximum character width (skipping ANSI codes)
        int chars = count_visible_chars(line);
        if (chars > max_line_chars) max_line_chars = chars;

        tmpnum++;
    }
    fclose(fp);

    if (tmpnum == 0) return;

    // ---- Now we know the canvas dimensions. Allocate it. -------------
    canvas_rows = tmpnum;
    canvas_cols = max_line_chars;
    canvas = malloc(canvas_rows * canvas_cols * CELL_SIZE);
    cell_colors = malloc(canvas_rows * canvas_cols);
    // Initialise every cell to a space character
    for (int i = 0; i < canvas_rows * canvas_cols; i++) {
        canvas[i][0] = ' ';
        canvas[i][1] = '\0';
        cell_colors[i] = DEFAULT_COLOR;
    }

    // ---- Fill the canvas from the temporary lines. ------------------
    //     Parses ANSI colour codes so a saved file re-opens with the
    //     correct colours; plain text gets default colour 7.
    for (int r = 0; r < tmpnum; r++) {
        fill_row(tmp[r], r);
        free(tmp[r]);
    }
    free(tmp);
}

/* ----------------------------------------------------------------- */
/*  Draw the entire screen — row by row to avoid flicker              */
/* ----------------------------------------------------------------- */

static void render(void) {
    // Query the current terminal size at draw time. This means we
    // automatically adapt to window resize on every frame.
    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    int screen_rows = ws.ws_row;
    int screen_cols = ws.ws_col;

    // Reserve the last row for the toolbar.
    int avail_rows = screen_rows - 1;
    int draw_rows = (canvas_rows < avail_rows) ? canvas_rows : avail_rows;

    for (int r = 0; r < draw_rows; r++) {
        // Move to start of row, clear it, then build & print the line.
        printf(FMT_CURSOR_ROW CLR_EOL, r + 1);

        // Build a line string from the cells of this row.
        // Emit ANSI colour codes whenever the colour changes.
        char linebuf[8192];
        int pos = 0;
        int last_color = -1;
        int max_cells = (canvas_cols < screen_cols) ? canvas_cols : screen_cols;

        for (int c = 0; c < max_cells; c++) {
            int idx = r * canvas_cols + c;
            int color = cell_colors ? cell_colors[idx] : DEFAULT_COLOR;
            if (color != last_color) {
                pos += snprintf(linebuf + pos, sizeof(linebuf) - pos,
                                FMT_SET_COLOR, ansi_colors[color]);
                last_color = color;
            }
            char *cell = canvas[idx];
            while (*cell && pos < (int)sizeof(linebuf) - 4) {
                linebuf[pos++] = *cell++;
            }
        }
        linebuf[pos] = '\0';

        printf("%s" RESET_COLOR, linebuf);
    }

    // Clear any remaining rows below the canvas (including toolbar row).
    for (int r = draw_rows; r <= screen_rows; r++) {
        printf(FMT_CURSOR_ROW CLR_EOL, r + 1);
    }

    // Draw the toolbar at the bottom of the terminal (reverse video).
    printf(FMT_CURSOR_ROW REV_ON, screen_rows);
    if (typing_mode && typing_x >= 0) {
        printf(" Type @ %d,%d " REV_OFF RESET_COLOR "  |  ^C:quit",
               typing_x, typing_y);
    } else if (typing_mode) {
        printf(" Type: click canvas " REV_OFF RESET_COLOR "  |  ^C:quit");
    } else {
        printf(" Brush: " FMT_SET_COLOR " %s " REV_OFF RESET_COLOR
               "  %s  |  0-9:color  ^S:save  ^Z:undo  ^Y:redo  ^T:type  ^C:quit",
               ansi_colors[brush_color], brush, color_names[brush_color]);
    }

    // Draw a solid cursor square on the canvas (reverse video block).
    if (typing_mode && typing_x >= 0 && typing_y >= 0) {
        int tx = typing_x + 1, ty = typing_y + 1;
        if (tx <= screen_cols && ty < screen_rows) {
            printf(FMT_CURSOR_POS FMT_SET_COLOR REV_ON " " REV_OFF RESET_COLOR,
                   ty, tx, ansi_colors[brush_color]);
        }
    } else if (mx >= 1 && my >= 1 && mx <= screen_cols && my < screen_rows) {
        printf(FMT_CURSOR_POS FMT_SET_COLOR REV_ON " " REV_OFF RESET_COLOR,
               my, mx, ansi_colors[brush_color]);
    }

    fflush(stdout);
}

/* ----------------------------------------------------------------- */
/*  Parse an SGR mouse report from stdin                              */
/* ----------------------------------------------------------------- */

static void handleMouseEvent(void) {
    // We already consumed the leading ESC. Read the next two bytes.
    char hdr[2];
    if (read(STDIN_FILENO, hdr, 1) != 1) return;
    if (hdr[0] != '[') return;
    if (read(STDIN_FILENO, hdr + 1, 1) != 1) return;
    if (hdr[1] != '<') return;

    // Read the "button;col;row" payload until the terminating M or m.
    // Uppercase M = press/drag, lowercase m = release.
    char buf[64];
    int bl = 0;
    int is_release = 0;
    while (bl < 63) {
        unsigned char ch;
        if (read(STDIN_FILENO, &ch, 1) != 1) return;
        if (ch == 'M') break;
        if (ch == 'm') { is_release = 1; break; }
        buf[bl++] = ch;
    }
    buf[bl] = 0;

    if (is_release) {
        mouse_seen = 1;
        is_drawing = 0;
        return;
    }

    int button, x, y;
    if (sscanf(buf, "%d;%d;%d", &button, &x, &y) != 3) return;

    mouse_seen = 1;
    mx = x; my = y;
    int cx = x - 1, cy = y - 1;
    int in_bounds = cx >= 0 && cx < canvas_cols && cy >= 0 && cy < canvas_rows;

    // In typing mode, a click positions the cursor instead of painting.
    if (typing_mode && button == 0 && in_bounds) {
        typing_x = typing_anchor_x = cx;
        typing_y = typing_anchor_y = cy;
        return;
    }

    // button 0 = press, 32 = drag, 35 = release with motion
    if (button == 0) {
        if (!in_bounds) return;
        push_undo();
        is_drawing = 1;
        last_paint_x = cx;
        last_paint_y = cy;
        paint_cell(cx, cy);
    } else if (button == 32) {
        if (is_drawing && (cx != last_paint_x || cy != last_paint_y)) {
            paint_line(last_paint_x, last_paint_y, cx, cy);
            last_paint_x = cx;
            last_paint_y = cy;
        }
    } else if (button == 35) {
        is_drawing = 0;
    }
}

/* ----------------------------------------------------------------- */
/*  Handle a regular key press — set brush character or colour        */
/* ----------------------------------------------------------------- */

// Read exactly N bytes with blocking semantics. Temporarily sets VMIN=1
// so we don't time out reading continuation bytes from the Character Viewer.
static int readn(unsigned char *buf, int n) {
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_cc[VMIN] = n;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    int got = 0;
    while (got < n) {
        int r = read(STDIN_FILENO, buf + got, n - got);
        if (r <= 0) break;
        got += r;
    }
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    return got;
}

static void handleKey(unsigned char c) {
    // Digit keys 0-9 set the brush colour.
    if (c >= '0' && c <= '9') {
        brush_color = c - '0';
        return;
    }

    // Regular key: set it as the active brush character.
    // Multi-byte UTF-8 (from the Character Viewer) is handled
    // by reading continuation bytes.
    unsigned char uc = c;
    int len = utf8_len(uc);
    if (len == 1 && uc >= ASCII_MIN && uc <= ASCII_MAX) {
        brush[0] = c;
        brush[1] = '\0';
    } else if (len > 1) {
        unsigned char tail[4];
        int got = readn(tail, len - 1);
        if (got == len - 1 && valid_utf8_tail(tail, len - 1)) {
            char tmp[CELL_SIZE];
            tmp[0] = c;
            for (int i = 0; i < len - 1; i++) tmp[i + 1] = tail[i];
            tmp[len] = '\0';
            strcpy(brush, tmp);
        }
    }
}

/* ----------------------------------------------------------------- */
/*  Save the canvas back to the file it was loaded from               */
/*  Writes ANSI colour codes inline, so the file is self-contained:   */
/*  - cat <file> shows colours in any ANSI-capable terminal           */
/*  - reopening in nanopaint restores both characters and colours     */
/* ----------------------------------------------------------------- */

static void saveFile(void) {
    const char *path = filepath ? filepath : "untitled.txt";

    FILE *fp = fopen(path, "w");
    if (!fp) return;

    for (int r = 0; r < canvas_rows; r++) {
        int last_color = -1;
        for (int c = 0; c < canvas_cols; c++) {
            int idx = r * canvas_cols + c;
            int color = cell_colors ? cell_colors[idx] : DEFAULT_COLOR;
            if (color != last_color) {
                fprintf(fp, FMT_SET_COLOR, ansi_colors[color]);
                last_color = color;
            }
            char *cell = canvas[idx];
            fputs(cell, fp);
        }
        fputc('\n', fp);
    }
    fclose(fp);

    filepath = path;
}

/* ----------------------------------------------------------------- */
/*  Command handlers                                                  */
/* ----------------------------------------------------------------- */

static void cmd_quit(void) { quit = 1; }
static void cmd_save(void) { saveFile(); }
static void cmd_type(void) {
    push_undo();
    typing_mode = 1;
    typing_x = typing_y = -1;
    typing_history_count = 0;
}

// The command table: each entry maps a key byte to a named action.
// To add a new command, add an entry here and define the handler above.
static const struct Command commands[] = {
    { KEY_CTRL_C, "C", "Quit",  cmd_quit },
    { KEY_CTRL_S, "S", "Save",  cmd_save },
    { KEY_CTRL_Z, "Z", "Undo",  undo     },
    { KEY_CTRL_Y, "Y", "Redo",  redo     },
    { KEY_CTRL_T, "T", "Type",  cmd_type },
    { 0,          NULL, NULL,   NULL      }
};

/* ----------------------------------------------------------------- */
/*  Typing-mode key handler                                           */
/* ----------------------------------------------------------------- */

// Save the current cell's contents into the typing history so that a
// subsequent backspace can restore it.
static void save_typing_cell(void) {
    if (typing_history_count >= MAX_TYPING_HISTORY) return;
    struct TypingEntry *e = &typing_history[typing_history_count];
    e->x = typing_x;
    e->y = typing_y;
    memcpy(e->old_char, &canvas[typing_y * canvas_cols + typing_x], CELL_SIZE);
    e->old_color = cell_colors[typing_y * canvas_cols + typing_x];
    e->is_newline = 0;
    typing_history_count++;
}

// Write a character (a NUL-terminated UTF-8 string in *s*, at most
// CELL_SIZE-1 displayable bytes) into the current typing cell and advance
// the cursor.
static void write_typing_cell(const char *s) {
    int idx = typing_y * canvas_cols + typing_x;
    strcpy(canvas[idx], s);
    cell_colors[idx] = brush_color;
    typing_x++;
}

static void handleTyping(unsigned char c) {
    if (typing_x < 0 || typing_y < 0) return;

    if (c == '\r' || c == '\n') {
        if (typing_history_count < MAX_TYPING_HISTORY) {
            struct TypingEntry *e = &typing_history[typing_history_count];
            e->x = typing_x;
            e->y = typing_y;
            e->is_newline = 1;
            typing_history_count++;
        }
        typing_y++;
        if (typing_y >= canvas_rows) typing_y = canvas_rows - 1;
        typing_x = typing_anchor_x;
    } else if (c == KEY_BACKSPACE) {
        if (typing_history_count > 0) {
            typing_history_count--;
            struct TypingEntry *e = &typing_history[typing_history_count];
            if (!e->is_newline) {
                memcpy(&canvas[e->y * canvas_cols + e->x], e->old_char, CELL_SIZE);
                cell_colors[e->y * canvas_cols + e->x] = e->old_color;
            }
            typing_x = e->x;
            typing_y = e->y;
        }
    } else if (c >= ASCII_MIN && c <= ASCII_MAX) {
        if (typing_x >= canvas_cols) return;
        char buf[CELL_SIZE] = { c, '\0' };
        save_typing_cell();
        write_typing_cell(buf);
    } else {
        unsigned char uc = c;
        int len = utf8_len(uc);
        if (len <= 1 || typing_x >= canvas_cols) return;
        unsigned char tail[4];
        if (readn(tail, len - 1) != len - 1) return;
        if (!valid_utf8_tail(tail, len - 1)) return;
        char buf[CELL_SIZE] = { '\0' };
        buf[0] = c;
        for (int i = 0; i < len - 1; i++) buf[i + 1] = tail[i];
        save_typing_cell();
        write_typing_cell(buf);
    }
}

/* ----------------------------------------------------------------- */
/*  Main loop — redraw continuously, handle input when it arrives    */
/* ----------------------------------------------------------------- */

static void mainLoop(void) {
    while (!quit) {
        render();

        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) == 0) continue;

        // In typing mode, keyboard input draws onto the canvas.
        // ESC exits typing mode; a standalone ESC is detected when
        // handleMouseEvent returns without seeing a valid SGR report.
        if (typing_mode) {
            if (c == KEY_ESC) {
                mouse_seen = 0;
                handleMouseEvent();
                if (!mouse_seen) {
                    typing_history_count = 0;
                    typing_mode = 0;
                    continue;
                }
                continue;
            }
            handleTyping(c);
            continue;
        }

        // Dispatch through command table first.
        int cmd_handled = 0;
        for (int i = 0; commands[i].key; i++) {
            if (c == commands[i].key) {
                commands[i].handler();
                cmd_handled = 1;
                break;
            }
        }
        if (cmd_handled) continue;
        if (quit) break;

        // ESC starts an SGR mouse report; anything else is a key press
        if (c == KEY_ESC) {
            handleMouseEvent();
        } else {
            handleKey(c);
        }
    }
}

/* ----------------------------------------------------------------- */
/*  Entry point                                                      */
/* ----------------------------------------------------------------- */

int main(int argc, char **argv) {
    enableRawMode();
    atexit(disableRawMode);
    signal(SIGINT, handler);
    signal(SIGTERM, handler);
    signal(SIGSEGV, handler);
    signal(SIGBUS, handler);
    signal(SIGABRT, handler);

    initTerminal();

    // Always start with a blank canvas the size of the terminal.
    // If a file is given, its content is overlaid on top.
    initBlankCanvas();
    if (argc > 1) {
        filepath = argv[1];
        loadFile(argv[1]);
    }

    mainLoop();

    return 0;
}