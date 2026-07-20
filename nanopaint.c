/*
 * nanopaint
 *
 * In progress: Terminal paint application
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
/*  Terminal state                                                   */
/* ----------------------------------------------------------------- */

static struct termios orig_tios;       /* saved terminal attributes */
static volatile sig_atomic_t quit;     /* exit flag for signal handler */

/* ----------------------------------------------------------------- */
/*  File path for saving                                              */
/* ----------------------------------------------------------------- */

static const char *filepath = NULL;

/* ----------------------------------------------------------------- */
/*  Canvas — a 2D grid of characters (cell = char[5])                 */
/* ----------------------------------------------------------------- */
/*  Each cell holds a single character, up to 4 bytes (UTF-8) plus   */
/*  a null terminator. This means emojis are supported.               */
/* ----------------------------------------------------------------- */

static char (*canvas)[5] = NULL;       /* the cell grid */
static int canvas_rows = 0;            /* number of rows in canvas */
static int canvas_cols = 0;            /* number of columns in canvas */

/* ----------------------------------------------------------------- */
/*  Cursor position — where the mouse pointer is on screen           */
/* ----------------------------------------------------------------- */

static int mx = -1, my = -1;          /* 1-based, -1 = hasn't moved yet */

/* ----------------------------------------------------------------- */
/*  Brush — the character and colour painted on click-drag            */
/* ----------------------------------------------------------------- */

static char brush[5] = "#";
static int brush_color = 7;           /* 0-9, 7 = white */

/* ----------------------------------------------------------------- */
/*  Drawing state                                                     */
/* ----------------------------------------------------------------- */

static int is_drawing = 0;

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

/* ----------------------------------------------------------------- */
/*  UTF-8 helpers                                                     */
/* ----------------------------------------------------------------- */

static int utf8_len(unsigned char c) {
    if (c >= 0xF0) return 4;   /* 4-byte character */
    if (c >= 0xE0) return 3;   /* 3-byte character */
    if (c >= 0xC0) return 2;   /* 2-byte character */
    return 1;                   /* 1-byte ASCII */
}

// Return the number of UTF-8 characters in a null-terminated string.
static int utf8_strlen(const char *s) {
    int n = 0;
    while (*s) {
        s += utf8_len((unsigned char)*s);
        n++;
    }
    return n;
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

/* ----------------------------------------------------------------- */
/*  Switch terminal to raw mode                                       */
/* ----------------------------------------------------------------- */

static void enableRawMode(void) {
    tcgetattr(STDIN_FILENO, &orig_tios);
    struct termios raw = orig_tios;

    // c_lflag (local mode flags) — control how the terminal driver processes
    // input before your program sees it:
    // ECHO      Automatically print every character the user types.
    //           Off = you control what's displayed.
    // ICANON    Canonical (line-buffered) mode.
    //           Off = each byte delivered immediately, no Enter needed.
    // IEXTEN    Enables implementation-defined special characters
    //           (like Ctrl+V for literal next on macOS/Linux).
    //           Off = no special processing.
    // ISIG      Enables signal-generating keys (Ctrl+C → SIGINT,
    //           Ctrl+Z → SIGTSTP). Off = you handle those keys yourself.
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    // c_iflag (input flags):
    // BRKINT    Break condition sends SIGINT. Off = ignore.
    // ICRNL     Translates CR (carriage return, \r) to NL (\n).
    //           Off = we see the actual byte.
    // INPCK     Enables parity checking. Off = ignore parity.
    // ISTRIP    Strips the 8th bit off each byte.
    //           Off = we get full 8-bit values (needed for UTF-8).
    // IXON      Enables XON/XOFF flow control (Ctrl+S/Ctrl+Q).
    //           Off = those keys pass through to our program.
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    // c_oflag (output flags):
    // OPOST     Enables output processing. Off = terminal won't translate
    //           \n to \r\n or mangle our escape sequences.
    raw.c_oflag &= ~(OPOST);

    // c_cc (control characters):
    // VMIN = 0  Minimum bytes before read() returns.
    //           0 = return immediately on timeout.
    // VTIME = 1 Timeout in deciseconds (tenths of seconds). 1 = 100ms.
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

/* ----------------------------------------------------------------- */
/*  Restore terminal to its original state                            */
/* ----------------------------------------------------------------- */

static void disableRawMode(void) {
    // Escape codes:
    // ESC[?25h      make cursor visible
    // ESC[?1003l    disables any-event mouse tracking
    // ESC[?1006l    disables SGR mouse encoding
    // ESC[?1049l    disables the alternative buffer
    write(STDOUT_FILENO, "\x1b[?25h\x1b[?1003l\x1b[?1006l\x1b[?1049l", 34);
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
    // ESC[?1049h   enables the alternative buffer
    // ESC[?25l     make cursor invisible
    // ESC[?1003h   enable any-event mouse tracking (motion without a button)
    // ESC[?1006h   enable SGR-encoded mouse reports (unlimited coords)
    // ESC[2J       erase entire screen
    // ESC[H        moves cursor to home position (0, 0)
    write(STDOUT_FILENO,
        "\x1b[?1049h\x1b[?25l\x1b[?1003h\x1b[?1006h\x1b[2J\x1b[H", 33);
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

/* ----------------------------------------------------------------- */
/*  Create a blank canvas the size of the terminal                     */
/*  Used when no file is given on the command line.                    */
/* ----------------------------------------------------------------- */

static void initBlankCanvas(void) {
    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    canvas_rows = ws.ws_row;
    canvas_cols = ws.ws_col;
    canvas = malloc(canvas_rows * canvas_cols * 5);
    cell_colors = malloc(canvas_rows * canvas_cols);
    for (int i = 0; i < canvas_rows * canvas_cols; i++) {
        canvas[i][0] = ' ';
        canvas[i][1] = '\0';
        cell_colors[i] = 7;
    }
}

/* ----------------------------------------------------------------- */
/*  Load a text file into the canvas                                  */
/* ----------------------------------------------------------------- */

static void loadFile(const char *path) {
    // ---- First pass: read all lines into a temporary array -----------
    //            and determine the canvas dimensions needed.
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char **tmp = NULL;       // temporary array of raw lines
    int tmpnum = 0;          // number of lines
    int tmpcap = 0;          // capacity of tmp array
    int max_line_chars = 0;  // max UTF-8 characters in any line
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

        // Update the maximum character width for this line
        int chars = utf8_strlen(line);
        if (chars > max_line_chars) max_line_chars = chars;

        tmpnum++;
    }
    fclose(fp);

    if (tmpnum == 0) return;

    // ---- Now we know the canvas dimensions. Allocate it. -------------
    canvas_rows = tmpnum;
    canvas_cols = max_line_chars;
    canvas = malloc(canvas_rows * canvas_cols * 5);
    cell_colors = malloc(canvas_rows * canvas_cols);
    // Initialise every cell to a space character
    for (int i = 0; i < canvas_rows * canvas_cols; i++) {
        canvas[i][0] = ' ';
        canvas[i][1] = '\0';
        cell_colors[i] = 7;
    }

    // ---- Fill the canvas from the temporary lines. ------------------
    for (int r = 0; r < tmpnum; r++) {
        const char *p = tmp[r];
        int c = 0;
        while (*p && c < canvas_cols) {
            utf8_cpy(canvas[r * canvas_cols + c], p);
            p = utf8_next(p);
            c++;
        }
        free(tmp[r]);
    }
    free(tmp);
}

/* ----------------------------------------------------------------- */
/*  Draw the entire screen from scratch                              */
/* ----------------------------------------------------------------- */

static void render(void) {
    // Clear the entire screen before drawing. Every frame starts from
    // a clean slate — old cursor blocks, stale content, anything left
    // from a previous resize is all erased. Then we draw exactly what
    // should be visible right now.
    // ESC[2J  = erase entire display
    // ESC[H   = move cursor home
    write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);

    // Query the current terminal size at draw time. This means we
    // automatically adapt to window resize on every frame.
    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    int screen_rows = ws.ws_row;
    int screen_cols = ws.ws_col;

    // Draw as many canvas rows as fit in the terminal window.
    int draw_rows = (canvas_rows < screen_rows) ? canvas_rows : screen_rows;
    for (int r = 0; r < draw_rows; r++) {
        // Build a line from the cells of this row; stop at screen_cols.
        // Each cell is a NUL-terminated UTF-8 string, so we just
        // concatenate them. Emit ANSI colour codes when the colour
        // changes between cells.
        char linebuf[8192];
        int pos = 0;
        int last_color = -1;
        int max_cells = (canvas_cols < screen_cols) ? canvas_cols : screen_cols;

        for (int c = 0; c < max_cells; c++) {
            int idx = r * canvas_cols + c;
            int color = cell_colors ? cell_colors[idx] : 7;
            if (color != last_color) {
                pos += snprintf(linebuf + pos, sizeof(linebuf) - pos,
                                "\x1b[%dm", ansi_colors[color]);
                last_color = color;
            }
            char *cell = canvas[idx];
            while (*cell && pos < (int)sizeof(linebuf) - 4) {
                linebuf[pos++] = *cell++;
            }
        }
        linebuf[pos] = '\0';

        // ESC[<row>;1H positions the cursor (1-based)
        printf("\x1b[%d;1H%s\x1b[39m", r + 1, linebuf);
    }

    // Draw the solid cursor square on top, in the current brush colour.
    // ESC[7m  = reverse video (space becomes a solid block)
    // ESC[27m = reverse video off
    if (mx >= 1 && my >= 1 && mx <= screen_cols && my <= screen_rows) {
        printf("\x1b[%d;%dH\x1b[%dm\x1b[7m \x1b[27m\x1b[39m",
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

    // Read the "button;col;row" payload until the terminating M or m
    char buf[64];
    int bl = 0;
    while (bl < 63) {
        unsigned char ch;
        if (read(STDIN_FILENO, &ch, 1) != 1) return;
        if (ch == 'M' || ch == 'm') break;
        buf[bl++] = ch;
    }
    buf[bl] = 0;

    int button, x, y;
    if (sscanf(buf, "%d;%d;%d", &button, &x, &y) != 3) return;

    // SGR coordinates are 1-based, same as our ANSI cursor positioning
    mx = x; my = y;

    int cx = x - 1, cy = y - 1;

    // button 0 = press, 32 = drag, 35 = release with motion, 'm' = release
    if (button == 0) {
        is_drawing = 1;
        paint_cell(cx, cy);
    } else if (button == 32) {
        if (is_drawing) paint_cell(cx, cy);
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
    if (len == 1 && uc >= 32 && uc <= 126) {
        brush[0] = c;
        brush[1] = '\0';
    } else if (len > 1) {
        unsigned char tail[4];
        int got = readn(tail, len - 1);
        if (got == len - 1) {
            int ok = 1;
            for (int i = 0; i < len - 1; i++) {
                if ((tail[i] & 0xC0) != 0x80) { ok = 0; break; }
            }
            if (ok) {
                char tmp[5];
                tmp[0] = c;
                for (int i = 0; i < len - 1; i++) tmp[i + 1] = tail[i];
                tmp[len] = '\0';
                strcpy(brush, tmp);
            }
        }
    }
}

/* ----------------------------------------------------------------- */
/*  Save the canvas back to the file it was loaded from               */
/* ----------------------------------------------------------------- */

static void saveFile(void) {
    const char *path = filepath ? filepath : "untitled.txt";

    FILE *fp = fopen(path, "w");
    if (!fp) return;

    for (int r = 0; r < canvas_rows; r++) {
        for (int c = 0; c < canvas_cols; c++) {
            char *cell = canvas[r * canvas_cols + c];
            fputs(cell, fp);
        }
        fputc('\n', fp);
    }
    fclose(fp);

    filepath = path;
}

/* ----------------------------------------------------------------- */
/*  Main loop — redraw continuously, handle input when it arrives    */
/* ----------------------------------------------------------------- */

static void mainLoop(void) {
    while (!quit) {
        // Render every frame. This is the "video" approach:
        //   - Terminal resize? It just works — we query the size each time.
        //   - Cursor moved? The next frame draws it at the new position.
        //   - No special-case logic for any of it.
        render();

        // Wait for input. VMIN=0, VTIME=1 means read() returns after
        // ~100ms if nothing is available — that's our "frame rate".
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) == 0) continue;

        // Ctrl+C to quit
        if (c == 3) break;

        // Ctrl+S to save (IXON is disabled, so it reaches us)
        if (c == 19) { saveFile(); continue; }

        // ESC starts an SGR mouse report; anything else is a key press
        if (c == '\x1b') {
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
