// includes
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/types.h>

// defines
#define CTRL_KEY(k) ((k) & 0x1f)
#define TAB_STOP 8
#define HIGHLIGHT_NUMBERS (1 << 0)
#define HIGHLIGHT_STRINGS (1 << 1)

// prototypes
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL,
    HOME,
    END,
    PAGE_UP,
    PAGE_DOWN,
};

enum editorHighlight {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

// data
struct editorRow {
    int idx;
    int size;
    int rsize; // size of contents of render
    char *chars;
    char *render; // for rendering tabs and other nonprintable chars like ^V
    unsigned char *highlight;
    int hl_open_comment;
};

struct editorConfig {
    int cx;
    int cy;
    int rx; // index into render of editorRow
    int rowoffset;
    int coloffset;
    int screenrows;
    int screencols;
    int numrows;
    char *filename;
    struct editorRow *row;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;
    struct editorSyntax *syntax;
    int dirty; // buffer modify flag
};

struct editorSyntax {
    char *filetype;
    char **filematch;
    char **keywords;
    char *singleline_comment_start;
    char *multiline_comment_start;
    char *multiline_comment_end;
    int flags;
};

// global instance of editor
struct editorConfig E;

// filetypes

char *C_HL_extensions[] = { ".c", ".h", ".cpp", NULL };
char *C_HL_keywords[] = {
  "switch", "if", "while", "for", "break", "continue", "return", "else",
  "struct", "union", "typedef", "static", "enum", "class", "case",
  "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
  "void|", NULL
};

// highlight database
struct editorSyntax HLDB[] = {
    {
        "c",
        C_HL_extensions,
        C_HL_keywords,
        "//",
        "/*",
        "*/",
        HIGHLIGHT_NUMBERS | HIGHLIGHT_STRINGS
    },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
        die("tcsetattr");
    }
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
        die("tcsetattr");
    }
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // Ctrl-m, Ctrl-s, Ctrl-q
    raw.c_oflag &= ~(OPOST); // disable carriage return
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG); // disable echo and canonical, Ctrl-v/o, SIGINT/SIGTSTP
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    }
}

int editorReadKey() {
    int nread;
    char c;

    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            die("read");
        }
    }

    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1 || read(STDIN_FILENO, &seq[1], 1) != 1) {
            return '\x1b';
        }

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return '\x1b';
                }

                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1':
                            return HOME;
                        case '3':
                            return DEL;
                        case '4':
                            return END;
                        case '5':
                            return PAGE_UP;
                        case '6':
                            return PAGE_DOWN;
                        case '7':
                            return HOME;
                        case '8':
                            return END;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A':
                        return ARROW_UP;
                    case 'B':
                        return ARROW_DOWN;
                    case 'C':
                        return ARROW_RIGHT;
                    case 'D':
                        return ARROW_LEFT;
                    case 'H':
                        return HOME;
                    case 'F':
                        return END;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H':
                    return HOME;
                case 'F':
                    return END;
            }
        }

        return '\x1b';
    } else {
        return c;
    }

    return c;
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return -1;
    }

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) {
            break;
        }
        if (buf[i] == 'R') {
            break;
        }
        ++i;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') {
        return -1;
    }
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
        return -1;
    }

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return -1;
        }

        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

// syntax highlighting

int is_separator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(struct editorRow *row) {
    row->highlight = realloc(row->highlight, row->rsize);
    memset(row->highlight, HL_NORMAL, row->rsize);

    if (E.syntax == NULL) {
        return;
    }

    char **keywords = E.syntax->keywords;

    char *scs = E.syntax->singleline_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end;

    int scs_len = 0;
    int mcs_len = 0;
    int mce_len = 0;
    if (scs) {
        scs_len = strlen(scs);
    }

    if (mcs) {
        mcs_len = strlen(mcs);
    }

    if (mce) {
        mce_len = strlen(mce);
    }

    int prev_sep = 1;
    int in_string = 0;
    int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);

    for (int i = 0; i < row->rsize; ++i) {
        char c = row->render[i];
        unsigned char prev_highlight = (i > 0) ? row->highlight[i - 1] : HL_NORMAL;

        if (scs_len && !in_string && !in_comment) {
            if (!strncmp(&row->render[i], scs, scs_len)) {
                memset(&row->highlight[i], HL_COMMENT, row->rsize - i);
                break;
            }
        }

        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->highlight[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, mce_len)) {
                    memset(&row->highlight[i], HL_MLCOMMENT, mce_len);
                    i += mce_len - 1;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                } else {
                    continue;
                }
            } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
                memset(&row->highlight[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len - 1;
                in_comment = 1;
                continue;
            }
        }

        if (E.syntax->flags & HIGHLIGHT_STRINGS) {
            if (in_string) {
                row->highlight[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->rsize) {
                    row->highlight[i + 1] = HL_STRING;
                    ++i;
                    continue;
                }

                if (c == in_string) {
                    in_string = 0;
                }

                prev_sep = 1;
                continue;
            } else {
                if (c == '"' || c == '\'') {
                    in_string = c;
                    row->highlight[i] = HL_STRING;
                    continue;
                }
            }
        }

        if (E.syntax->flags & HIGHLIGHT_NUMBERS) {
            if (isdigit(c) && (prev_sep || prev_highlight == HL_NUMBER) || (c == '.' && prev_highlight == HL_NUMBER)) {
                row->highlight[i] = HL_NUMBER;
                prev_sep = 0;
                continue;
            }
        }

        if (prev_sep) {
            int j;
            for (j = 0; keywords[j]; ++j) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                if (kw2) {
                    --klen;
                }

                if (!strncmp(&row->render[i], keywords[j], klen) && is_separator(row->render[i + klen])) {
                    memset(&row->highlight[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen - 1;
                    break;
                }
            }

            if (keywords[j] != NULL) {
                prev_sep = 0;
                --i;
                continue;
            }
        }

        prev_sep = is_separator(c);
    }

    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->idx + 1 < E.numrows) {
        editorUpdateSyntax(&E.row[row->idx + 1]);
    }
}

int editorSyntaxToColor(int highlight) {
    switch (highlight) {
        case HL_COMMENT:
        case HL_MLCOMMENT:
            return 36; // foreground cyan
        case HL_KEYWORD1:
            return 33; // foreground yellow
        case HL_KEYWORD2:
            return 32; // foreground green
        case HL_STRING:
            return 35; // foreground magenta
        case HL_NUMBER:
            return 31; // foreground red
        case HL_MATCH:
            return 34; // foreground blue
        default:
            return 37; // foreground white
    }
}

void editorSelectSyntaxHighlight() {
    E.syntax = NULL;
    if (E.filename == NULL) {
        return;
    }

    char *ext = strrchr(E.filename, '.');

    for (int i = 0; i < HLDB_ENTRIES; ++i) {
        struct editorSyntax *s = &HLDB[i];

        int j = 0;
        while (s->filematch[j]) {
            int is_ext = (s->filematch[j][0] == '.');
            if ((is_ext && ext && !strcmp(ext, s->filematch[j])) || (!is_ext && strstr(E.filename, s->filematch[j]))) {
                E.syntax = s;

                for (int filerow = 0; filerow < E.numrows; ++filerow) {
                    editorUpdateSyntax(&E.row[filerow]);
                }

                return;
            }
            ++j;
        }
    }
}

// row operations

int editorRowCxToRx(struct editorRow *row, int cx) {
    // convert char index to render index
    int rx = 0;
    for (int j = 0; j < cx; ++j) {
        if (row->chars[j] == '\t') {
            rx += (TAB_STOP - 1) - (rx % TAB_STOP);
        }

        ++rx;
    }

    return rx;
}

int editorRowRxToCx(struct editorRow *row, int rx) {
    int rx_current = 0;
    int cx;
    for (cx = 0; cx < row->size; ++cx) {
        if (row->chars[cx] == '\t') {
            rx_current += (TAB_STOP - 1) - (rx_current % TAB_STOP);
        }

        ++rx_current;

        if (rx_current > rx) {
            return cx;
        }
    }

    return cx;

}

void editorUpdateRow(struct editorRow *row) {
    int tabs = 0;
    for (int j = 0; j < row->size; ++j) {
        if (row->chars[j] == '\t') {
            ++tabs;
        }
    }

    free(row->render);
    row->render = malloc(row->size + tabs * (TAB_STOP - 1) + 1);

    int i = 0;
    for (int j = 0; j < row->size; ++j) {
        if (row->chars[j] == '\t') {
            // render tabs as spaces
            row->render[i] = ' ';
            ++i;

            while (i % TAB_STOP != 0) {
                row->render[i] = ' ';
                ++i;
            }
        } else {
            row ->render[i] = row->chars[j];
            ++i;
        }

    }

    row->render[i] = '\0';
    row->rsize = i;

    editorUpdateSyntax(row);
}

void editorInsertRow(int i, char *s, size_t len) {
    if (i < 0 || i > E.numrows) {
        return;
    }

    E.row = realloc(E.row, sizeof(struct editorRow) * (E.numrows + 1));
    memmove(&E.row[i + 1], &E.row[i], sizeof(struct editorRow) * (E.numrows - i));
    for (int j = i + 1; j <= E.numrows; ++j) {
        ++E.row[j].idx;
    }

    E.row[i].idx = i;

    E.row[i].size = len;
    E.row[i].chars = malloc(len + 1);
    memcpy(E.row[i].chars, s, len);
    E.row[i].chars[len] = '\0';

    E.row[i].rsize = 0;
    E.row[i].render = NULL;
    E.row[i].highlight = NULL;
    E.row[i].hl_open_comment = 0;
    editorUpdateRow(&E.row[i]);

    ++E.numrows;
    E.dirty = 1;
}

void editorFreeRow(struct editorRow *row) {
    free(row->render);
    free(row->chars);
    free(row->highlight);
}

void editorDeleteRow(int i) {
    if (i < 0 || i >= E.numrows) {
        return;
    }

    editorFreeRow(&E.row[i]);
    memmove(&E.row[i], &E.row[i + 1], sizeof(struct editorRow) * (E.numrows - i - 1));

    for (int j = i; j < E.numrows - 1; ++j) {
        --E.row[j].idx;
    }

    --E.numrows;
    E.dirty = 1;
}

void editorRowInsertChar(struct editorRow *row, int i, int c) {
    if (i < 0 || i > row->size) {
        i = row->size;
    }
    row->chars = realloc(row->chars, row->size + 2);
    // allocate byte for row->chars
    memmove(&row->chars[i + 1], &row->chars[i], row->size - i + 1);
    ++row->size;
    row->chars[i] = c;
    editorUpdateRow(row);
    E.dirty = 1;
}

void editorRowAppendString(struct editorRow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty = 1;
}

void editorRowDeleteChar(struct editorRow *row, int i) {
    if (i < 0 || i >= row->size) {
        return;
    }

    memmove(&row->chars[i], &row->chars[i + 1], row->size - i);
    --row->size;
    editorUpdateRow(row);
    E.dirty = 1;
}

// editor operations

void editorInsertChar(int c) {
    if (E.cy == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }

    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    ++E.cx;
}

void editorInsertNewline() {
    if (E.cx == 0) {
        // insert blank newline
        editorInsertRow(E.cy, "", 0);
    } else {
        // split up into 2 rows
        struct editorRow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }

    ++E.cy;
    E.cx = 0;
}

void editorDeleteChar() {
    if (E.cy == E.numrows || (E.cx == 0 && E.cy == 0)) {
        return;
    }

    struct editorRow *row = &E.row[E.cy];
    if (E.cx > 0) {
        editorRowDeleteChar(row, E.cx - 1);
        --E.cx;
    } else {
        // delete row and go to previous
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDeleteRow(E.cy);
        --E.cy;
    }
}

void editorSetStatusMessage(const char *fmt, ...) {
    // printf style interface for sending messages to text editor
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

// file i/o

char *editorRowsToString(int *buflen) {
    // get length of string to malloc
    int totalLen = 0;
    for (int i = 0; i < E.numrows; ++i) {
        totalLen += E.row[i].size + 1;
    }

    *buflen = totalLen;

    char *buf = malloc(totalLen);
    char *p = buf;

    // loop through each row, delimit by new lines
    for (int i = 0; i < E.numrows; ++i) {
        memcpy(p, E.row[i].chars, E.row[i].size);
        p += E.row[i].size;
        *p = '\n';
        ++p;
    }

    return buf;
}

void editorOpen(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);

    editorSelectSyntaxHighlight();

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        die("fopen");
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
            --linelen;
        }

        editorInsertRow(E.numrows, line, linelen);
        /*
        E.row.size = linelen;
        E.row.chars = malloc(linelen + 1);
        memcpy(E.row.chars, line, linelen);
        E.row.chars[linelen] = '\0';
        E.numrows = 1;
        */
    }

    /* char *line = "Hello, world!"; */
    /* ssize_t linelen = 13; */
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave() {
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
        editorSelectSyntaxHighlight();
    }

    int len;
    char *buf = editorRowsToString(&len);

    // overwrite file without potential data loss
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        // set filesize to new length
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;

                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }

        close(fd);
    }

    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

// find

void editorFindCallback(char *query, int key) {
    // setup searching forward/backward
    static int last_match = -1;
    static int direction = 1;

    // restore syntax highlighting after search
    static int saved_highlight_line;
    static char *saved_highlight = NULL;

    if (saved_highlight) {
        memcpy(E.row[saved_highlight_line].highlight, saved_highlight, E.row[saved_highlight_line].rsize);
        free(saved_highlight);
        saved_highlight = NULL;
    }

    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1) {
        direction = 1;
    }

    int current = last_match;
    for (int i = 0; i < E.numrows; ++i) {
        current += direction;
        if (current == -1) {
            current = E.numrows - 1;
        } else if (current == E.numrows) {
            current = 0;
        }

        struct editorRow *row = &E.row[current];
        char *match = strstr(row->render, query);
        if (match) {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, match - row->render);
            E.rowoffset = E.numrows;

            saved_highlight_line = current;
            saved_highlight = malloc(row->rsize);
            memcpy(saved_highlight, row->highlight, row->rsize);

            memset(&row->highlight[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editorFind() {
    // store cursor location data so we can restore to it if search is aborted
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloffset = E.coloffset;
    int saved_rowoffset = E.rowoffset;

    char *query = editorPrompt("Search: %s (Use ESC/Enter/Arrows)", editorFindCallback);

    if (query) {
        free(query);
    } else {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloffset = saved_coloffset;
        E.rowoffset = saved_rowoffset;
    }
}

// append buffer

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) {
        return;
    }

    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

// output

void editorScroll() {
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    // vertical scroll
    if (E.cy < E.rowoffset) {
        E.rowoffset = E.cy;
    }

    if (E.cy >= E.rowoffset + E.screenrows) {
        E.rowoffset = E.cy - E.screenrows + 1;
    }

    // horizontal scroll
    if (E.rx < E.coloffset) {
        E.coloffset = E.rx;
    }
    if (E.rx >= E.coloffset + E.screencols) {
        E.coloffset = E.rx - E.screencols + 1;
    }
}

void editorDrawRows(struct abuf *ab) {
    for (int y = 0; y < E.screenrows; ++y) {
        if (y + E.rowoffset >= E.numrows) {
            abAppend(ab, "~", 1); // draw tildes for empty lines like vim
        } else {
            // draw row
            int len = E.row[y + E.rowoffset].rsize - E.coloffset;
            if (len < 0) {
                len = 0;
            }

            if (len > E.screencols) {
                len = E.screencols;
            }

            char *c = &E.row[y + E.rowoffset].render[E.coloffset];
            unsigned char *highlight = &E.row[y + E.rowoffset].highlight[E.coloffset];
            int current_color = -1;

            for (int i = 0; i < len; ++i) {
                if (iscntrl(c[i])) {
                    char sym = (c[i] <= 26) ? '@' + c[i] : '?';
                    abAppend(ab, "\x1b[7m", 4);
                    abAppend(ab, &sym, 1);
                    abAppend(ab, "\x1b[m", 3);
                    if (current_color != -1) {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        abAppend(ab, buf, clen);
                    }
                } else if (highlight[i] == HL_NORMAL) {
                    if (current_color != -1) {
                        abAppend(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }

                    abAppend(ab, &c[i], 1);
                } else {
                    int color = editorSyntaxToColor(highlight[i]);
                    if (color != current_color) {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, clen);
                    }

                    abAppend(ab, &c[i], 1);
                }
            }
            abAppend(ab, "\x1b[39m", 5);

            /* abAppend(ab, &E.row[y + E.rowoffset].render[E.coloffset], len); */
        }

        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4);
    char status[80];
    char rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "[No Name]", E.numrows, E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d", E.syntax ? E.syntax->filetype : "no ft", E.cy + 1, E.numrows);
    if (len > E.screencols) {
        len = E.screencols;
    }
    abAppend(ab, status, len);

    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            ++len;
        }
    }

    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 3);
}

void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) {
        msglen = E.screencols;
    }

    if (msglen && time(NULL) - E.statusmsg_time < 5) {
        abAppend(ab, E.statusmsg, msglen);
    }
}

void editorRefreshScreen() {
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    // adjust for scrolling and cursor movement
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoffset) + 1, (E.rx - E.coloffset) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}



// input

char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    // continually set status message, refresh screen. and wait for input
    for (;;) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL || c == BACKSPACE) {
            if (buflen != 0) {
                --buflen;
                buf[buflen] = '\0';
            }
        } else if (c == '\x1b') {
            // cancel input with escape
            editorSetStatusMessage("");
            if (callback) {
                callback(buf, c);
            }

            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                if (callback) {
                    callback(buf, c);
                }

                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize += 2;
                buf = realloc(buf, bufsize);
            }

            buf[buflen] = c;
            ++buflen;
            buf[buflen] = '\0';
        }

        if (callback) {
            callback(buf, c);
        }
    }
}

void editorMoveCursor(int key) {
    struct editorRow *row = NULL;

    // check if cursor is on an actual line
    if (E.cy < E.numrows) {
        row = &E.row[E.cy];
    }

    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                --E.cx;
            } else if (E.cy > 0) {
                // wrap to end of previous line
                --E.cy;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            // check if cx is left to the end of the row
            if (row && E.cx < row->size) {
                ++E.cx;
            } else if (row && E.cx == row->size) {
                // wrap to beginning of next line
                ++E.cy;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                --E.cy;
            }
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows) {
                ++E.cy;
            }
            break;
    }

    // correct cx if it was placed beyond its line
    row = NULL;
    if (E.cy < E.numrows) {
        row = &E.row[E.cy];
    }
    int rowlength = 0;
    if (row) {
        rowlength = row->size;
    }

    if (E.cx > rowlength) {
        E.cx = rowlength;
    }
}

void editorProcessKeypress() {
    static int quit_times = 3;

    int c = editorReadKey();

    switch (c) {
        case '\r': // enter key
            editorInsertNewline();
            break;
        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0) {
                editorSetStatusMessage("Warning! Unsaved changes. Press Ctrl-Q %d more times to quit.", quit_times);
                --quit_times;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case CTRL_KEY('s'):
            editorSave();
            break;
        case HOME:
            E.cx = 0;
            break;
        case END:
            if (E.cy < E.numrows) {
                E.cx = E.row[E.cy].size;
            }
            break;
        case CTRL_KEY('f'):
            editorFind();
            break;
        case BACKSPACE:
            editorDeleteChar();
            break;
        /* case CTRL_KEY('h'): */
        case DEL:
            editorMoveCursor(ARROW_RIGHT);
            editorDeleteChar();
            break;
        case PAGE_UP: {
            E.cy = E.rowoffset;

            int times = E.screenrows;
            while (times > 0) {
                editorMoveCursor(ARROW_UP);
                --times;
            }
            break;
        }
        case PAGE_DOWN: {
            E.cy = E.rowoffset + E.screenrows - 1;
            if (E.cy > E.numrows) {
                E.cy = E.numrows;
            }

            int times = E.screenrows;
            while (times > 0) {
                editorMoveCursor(ARROW_DOWN);
                --times;
            }
            break;
        }
        case CTRL_KEY('p'):
            editorMoveCursor(ARROW_UP);
            break;
        case CTRL_KEY('n'):
            editorMoveCursor(ARROW_DOWN);
            break;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
        case CTRL_KEY('l'):
        case '\x1b':
            break;
        default:
            editorInsertChar(c);
            break;
    }

    quit_times = 3;
}

// init

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoffset = 0;
    E.coloffset = 0;
    E.numrows = 0;
    E.filename = NULL;
    E.row = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.syntax = NULL;
    E.dirty = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
        die("getWindowSize");
    }

    E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-Q = quit, Ctrl-S = save, CTRL-F = find");

    // main event loop
    // continually refresh screen and process inputs
    for (;;) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

   return 0;
}
