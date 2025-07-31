#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>
#define CTRL_KEY(k) ((k) & 0x1f)

void editor_set_status_message(const char *msg, ...);

typedef struct erow {
    int size;
    int rsize;
    char *chars;
    char *torender;
} erow;

enum editor_key {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_UP = 1001,
    ARROW_RIGHT = 1002,
    ARROW_DOWN = 1003
};

struct editorConfig {
    int cx, cy;
    int screenrows;
    int screencols;
    int rowoff;
    int numrows;
    int reserved_x;
    int indent_x;
    int reserved_y;
    int diffs;
    char *filename;
    char statusmsg[80];
    erow *row;
    struct termios orig_terminal_config;
} E;

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(EXIT_FAILURE);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_terminal_config) == -1)
        die("tcsetattr");
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_terminal_config) == -1)
        die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.orig_terminal_config;
    raw.c_iflag &= ~(ICRNL | IXON);
    // Disables software flow control keys ctrl+s and ctrl+q
    raw.c_oflag &= ~(OPOST);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN); 
    // Removes echoing chars and turns off canonical mode (now reads char by char)
    // Turns off signals sent by ctrl+c or ctrl+z
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) 
        die("tcsetattr");
}

int editor_read_key() {
    char c;
    int nstatus;
    while ((nstatus = read(STDIN_FILENO, &c, 1)) != 1)
        if (nstatus == -1 && errno != EAGAIN)
            die("editor read key");

    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1 || read(STDIN_FILENO, &seq[1], 1) != 1) 
            return '\x1b';
        if (seq[0] == '[')
            switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
            }
        return '\x1b';
    } else
        return c;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
        return -1;
    else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

void editor_update_row(erow *row) {
    free(row->torender);
    row->torender = malloc(row->size + 1);
    int idx = 0;
    for (int j = 0; j < row->size; j++)
        row->torender[idx++] = row->chars[j];
    row->torender[idx] = '\0';
    row->rsize = idx;
}

void editor_append_row(char *line, int linelen) {
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    int end = E.numrows;
    E.row[end].size = linelen;
    E.row[end].chars = malloc(linelen + 1);
    memcpy(E.row[end].chars, line, linelen);
    E.row[end].chars[linelen] = '\0';
    E.row[end].rsize = 0;
    E.row[end].torender = NULL;

    editor_update_row(&E.row[end]);

    E.numrows++;
}

void editor_row_insert_char(erow *row, int at, int c) {
    at -= E.reserved_x;
    if (at < 0 || at > row->size) 
        at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    row->chars[row->size] = '\0';
    editor_update_row(row);
}

void editorInsertChar(int c) {
    if (E.cy == E.numrows)
        editor_append_row("", 0);
    editor_row_insert_char(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editor_row_del_char(erow *row, int at) {
    at -= E.reserved_x;
    if (at < 0 || at >= row->size) 
        return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editor_update_row(row);
    E.diffs++;
}

void editor_del_char() {
    if (E.cy == E.numrows)
        return;
    erow *row = &E.row[E.cy];
    if (E.cx > E.reserved_x) {
        editor_row_del_char(row, E.cx - 1);
        E.cx--;
    }
}

char *editor_rows_to_string(int *buflen) {
    int total_len = 0;
    int j;
    for (j = 0; j < E.numrows; j++)
        total_len += E.row[j].size + 1;
    *buflen = total_len;
    char *buf = malloc(total_len);
    char *p = buf;
    for (j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p++ = '\n';
    }
    return buf;
}

void editorOpen(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editor_append_row(line, linelen);
    }
    free(line);
    fclose(fp);
}

void editor_save() {
    if (E.filename == NULL) return;
    int len;
    char *buf = editor_rows_to_string(&len);
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    ftruncate(fd, len);
    write(fd, buf, len);
    close(fd);
    editor_set_status_message("File Saved!");
    free(buf);
}

struct abuf {
    char *buf;
    int len;
};

void abAppend(struct abuf *ab, const char *s, int elen) {
    char *new = realloc(ab->buf, ab->len + elen);
    if (new == NULL) 
        return;
    memcpy(&new[ab->len], s, elen);
    ab->buf = new;
    ab->len += elen;
}

void abFree(struct abuf *ab) {
    free(ab->buf);
}

void editorScroll() {
    if (E.cy < E.rowoff)
        E.rowoff = E.cy;

    if (E.cy + E.reserved_y >= E.rowoff + E.screenrows)
        E.rowoff = E.cy + E.reserved_y - E.screenrows + 1;
}

void editor_draw_rows(struct abuf *wbatch) {
    abAppend(wbatch, "\r\n", 2);
    int filerow;
    for (int y = 1; y < E.screenrows - 1; y++) {
        filerow = y + E.rowoff;
        char curline[32];
        int curlinelen = snprintf(curline, sizeof(curline), "%4d ", filerow);
        abAppend(wbatch, curline, curlinelen);
        if (filerow > E.numrows) {
            if (E.numrows == 0 && y == 5) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "$ Bloc editor\r\n");
                abAppend(wbatch, welcome, welcomelen);
            } else {
                abAppend(wbatch, "\r\n", 3);
            }
        } else {
            int len = E.row[filerow-1].rsize;
            if (len > E.screencols) 
                len = E.screencols;
            abAppend(wbatch, E.row[filerow-1].torender, len);
            abAppend(wbatch, "\x1b[K", 3); // Erase the right part of the line
            abAppend(wbatch, "\r\n", 2);
        }
        abAppend(wbatch, "\x1b[K", 3); // Erase the right part of the line
    }
    abAppend(wbatch, "\x1b[K", 3);
}

void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines. Cy: %d, Cx: %d",
        E.filename ? E.filename : "[No Name]", E.numrows, E.cy, E.cx);
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
        E.cy + 1, E.numrows);
    if (len > E.screencols) 
        len = E.screencols;
    abAppend(ab, status, len);
    while (len < E.screencols) {
        if (E.screencols - len == rlen + 1) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3); // Reset text formatting
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4); // Invert background and foreground colors
    abAppend(ab, "\x1b[K", 3); // Clear the line before writing
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    abAppend(ab, E.statusmsg, msglen);
    while (msglen++ < E.screencols)
        abAppend(ab, " ", 1);
    abAppend(ab, "\x1b[m", 3); // Reset text formatting
}

void editor_set_status_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
}

void editor_refresh_screen() {
    editorScroll();
    struct abuf wbatch = {NULL, 0}; 

    abAppend(&wbatch, "\x1b[?25l", 6); // Hide cursor
    abAppend(&wbatch, "\x1b[H", 3); // Position the cursor at the start of the screen

    editor_draw_rows(&wbatch);
    editorDrawStatusBar(&wbatch);
    editorDrawMessageBar(&wbatch);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.rowoff + 2, E.cx + 1);
    abAppend(&wbatch, buf, strlen(buf));

    abAppend(&wbatch, "\x1b[?25h", 6); // Show cursor

    write(STDOUT_FILENO, wbatch.buf, wbatch.len); // Write everything at once
    abFree(&wbatch);
}

void editorMoveCursor(int key) {
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT:
            if (E.cx > 0)
                E.cx--; 
            else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size + E.reserved_x)
                E.cx++;
            else if (row && E.cx == row->size + E.reserved_x) {
                E.cy++;
                E.cx = E.reserved_x + E.indent_x;   
            }
            break;
        case ARROW_UP:
            if (E.cy != 0)
                E.cy--;
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows)
                E.cy++;
            break;
    }
}

void editor_process_key() {
    int c = editor_read_key();

    switch (c) {
        case '\r':
            /* TODO */
            break;
        case BACKSPACE:
            editor_del_char();
            break;
        case CTRL_KEY('s'):
            editor_save();
            break;
        case CTRL_KEY('e'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(EXIT_SUCCESS);
            break;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
        default:
            editorInsertChar(c);
            break;
    }
}

void init_editor() {
    E.reserved_x = 4;
    E.reserved_y = 2;
    E.indent_x = 1;
    E.cx = E.reserved_x + 1;
    E.cy = 0;
    E.numrows = 0;
    E.rowoff = 1;
    E.statusmsg[0] = '\0';
    E.row = NULL;
    E.diffs = 0;
    E.filename = NULL;
    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
    E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
    enableRawMode();
    init_editor();
    
    if (argc >= 2)
        editorOpen(argv[1]);

    editor_set_status_message("HELP: Ctrl-s = save, Ctrl-e = exit");
    while (1) {
        editor_refresh_screen();
        editor_process_key();
    }
    return 0;
}