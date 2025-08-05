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
char *editor_prompt(char *);


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

void disable_raw_mode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_terminal_config) == -1)
        die("tcsetattr");
}

void enable_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_terminal_config) == -1)
        die("tcgetattr");
    atexit(disable_raw_mode);

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

int get_window_size(int *rows, int *cols) {
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

void editor_insert_row(int at, char *line, int linelen) {
    if (at < 0 || at > E.numrows) return;
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = linelen;
    E.row[at].chars = malloc(linelen + 1);
    memcpy(E.row[at].chars, line, linelen);
    E.row[at].chars[linelen] = '\0';
    E.row[at].rsize = 0;
    E.row[at].torender = NULL;

    editor_update_row(&E.row[at]);

    E.numrows++;
    E.diffs++;
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

void editor_insert_char(int c) {
    if (E.cy == E.numrows)
        editor_insert_row(E.numrows, "", 0);
    editor_row_insert_char(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editor_insert_newline() {
    if (E.cx <= E.reserved_x) {
        editor_insert_row(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        int editorcursor = E.cx - E.reserved_x - E.indent_x + 1;
        editor_insert_row(E.cy + 1, &row->chars[editorcursor], row->size - editorcursor);
        row = &E.row[E.cy];
        row->size = editorcursor;
        row->chars[row->size] = '\0';
        editor_update_row(row);
    }
    E.cy++;
    E.cx = E.reserved_x + E.indent_x - 1;
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

void editor_join_rows() {
    if (E.cy == 0) 
        return;
    erow *prev = &E.row[E.cy - 1];
    erow *curr = &E.row[E.cy];
    prev->chars = realloc(prev->chars, prev->size + curr->size);
    for (int j = 0; j < curr->size; j++)
        prev->chars[prev->size + j] = curr->chars[j];
    prev->chars[prev->size + curr->size] = '\0';
    prev->size += curr->size;
    free(curr->chars);
    free(curr->torender);
    memmove(&E.row[E.cy], &E.row[E.cy + 1], sizeof(erow) * (E.numrows - E.cy - 1));
    E.numrows--;
    editor_update_row(prev);
    E.cy--;
}

void editor_del_char() {
    if (E.cy == E.numrows)
        return;
    erow *row = &E.row[E.cy];
    if (E.cx > E.reserved_x + E.indent_x - 1) {
        editor_row_del_char(row, E.cx - 1);
        E.cx--;
    } else if (E.cx == E.reserved_x + E.indent_x - 1) {
        editor_join_rows();
        E.cx = E.row[E.cy].size + E.reserved_x;
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

void editor_open(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editor_insert_row(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
}

void editor_save() { 
    if (E.filename == NULL)
        E.filename = editor_prompt("Save as: %s");

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

void ab_append(struct abuf *ab, const char *s, int elen) {
    char *new = realloc(ab->buf, ab->len + elen);
    if (new == NULL) 
        return;
    memcpy(&new[ab->len], s, elen);
    ab->buf = new;
    ab->len += elen;
}

void ab_free(struct abuf *ab) {
    free(ab->buf);
}

void editor_scroll() {
    if (E.cy < E.rowoff)
        E.rowoff = E.cy;

    if (E.cy + E.reserved_y >= E.rowoff + E.screenrows)
        E.rowoff = E.cy + E.reserved_y - E.screenrows + 1;
}

void editor_draw_rows(struct abuf *wbatch) {
    ab_append(wbatch, "\r\n", 2);
    int filerow;
    for (int y = 1; y < E.screenrows - 1; y++) {
        filerow = y + E.rowoff;
        char curline[32];
        int curlinelen = snprintf(curline, sizeof(curline), "%4d ", filerow);
        ab_append(wbatch, curline, curlinelen);
        if (filerow > E.numrows) {
            if (E.numrows == 0 && y == 5) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "$ Bloc editor\r\n");
                ab_append(wbatch, welcome, welcomelen);
            } else {
                ab_append(wbatch, "\r\n", 3);
            }
        } else {
            int len = E.row[filerow-1].rsize;
            if (len > E.screencols) 
                len = E.screencols;
            ab_append(wbatch, E.row[filerow-1].torender, len);
            ab_append(wbatch, "\x1b[K", 3); // Erase the right part of the line
            ab_append(wbatch, "\r\n", 2);
        }
        ab_append(wbatch, "\x1b[K", 3); // Erase the right part of the line
    }
    ab_append(wbatch, "\x1b[K", 3);
}

void editor_draw_status_bar(struct abuf *ab) {
    ab_append(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines. Cy: %d, Cx: %d",
        E.filename ? E.filename : "[No Name]", E.numrows, E.cy, E.cx);
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
        E.cy + 1, E.numrows);
    if (len > E.screencols) 
        len = E.screencols;
    ab_append(ab, status, len);
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            ab_append(ab, rstatus, rlen);
            break;
        } else {
            ab_append(ab, " ", 1);
            len++;
        }
    }
    ab_append(ab, "\x1b[m", 3); // Reset text formatting
    ab_append(ab, "\r\n", 2);
}

void editor_draw_message_bar(struct abuf *ab) {
    ab_append(ab, "\x1b[7m", 4); // Invert background and foreground colors
    ab_append(ab, "\x1b[K", 3); // Clear the line before writing
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    ab_append(ab, E.statusmsg, msglen);
    while (msglen++ < E.screencols)
        ab_append(ab, " ", 1);
    ab_append(ab, "\x1b[m", 3); // Reset text formatting
}

void editor_set_status_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
}

void editor_refresh_screen() {
    editor_scroll();
    struct abuf wbatch = {NULL, 0}; 

    ab_append(&wbatch, "\x1b[?25l", 6); // Hide cursor
    ab_append(&wbatch, "\x1b[H", 3); // Position the cursor at the start of the screen
    ab_append(&wbatch, "\x1b[32m", 5); // Color red

    editor_draw_rows(&wbatch);
    editor_draw_status_bar(&wbatch);
    editor_draw_message_bar(&wbatch);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.rowoff + 2, E.cx + 1);
    ab_append(&wbatch, buf, strlen(buf));

    ab_append(&wbatch, "\x1b[?25h", 6); // Show cursor

    write(STDOUT_FILENO, wbatch.buf, wbatch.len); // Write everything at once
    ab_free(&wbatch);
}

void editor_move() {
    char *nline = editor_prompt("Enter line: %s");
    if (nline != NULL) {
        int line = atoi(nline);
        if (line >= 0 && line < E.numrows) {
            E.cy = line;
            E.rowoff = line;  // scroll so it's visible
        } else {
            editor_set_status_message("Invalid line number.");
        }
        free(nline);
    }
}


char *editor_prompt(char *prompt) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);
    size_t buflen = 0;
    buf[0] = '\0';
    while (1) {
        editor_set_status_message(prompt, buf);
        editor_refresh_screen();
        int c = editor_read_key();
        if (c == '\x1b') {
            editor_set_status_message("");
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editor_set_status_message("");
                return buf;
            }
            } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
    }
}

void editor_move_cursor(int key) {
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT:
            if (E.cx > E.reserved_x)
                E.cx--; 
            else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size + E.reserved_x + E.indent_x;
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
            editor_insert_newline();
            break;
        case BACKSPACE:
            editor_del_char();
            break;
        case CTRL_KEY('l'):
            editor_move();
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
            editor_move_cursor(c);
            break;
        default:
            editor_insert_char(c);
            break;
    }
}

void init_editor() {
    E.reserved_x = 4;
    E.reserved_y = 2;
    E.indent_x = 1;
    E.cx = E.reserved_x;
    E.cy = 0;
    E.numrows = 0;
    E.rowoff = 1;
    E.statusmsg[0] = '\0';
    E.row = NULL;
    E.diffs = 0;
    E.filename = NULL;
    if (get_window_size(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
    E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
    enable_raw_mode();
    init_editor();
    
    if (argc >= 2)
        editor_open(argv[1]);

    editor_set_status_message("HELP: Ctrl-s = save, Ctrl-e = exit, Ctrl-l = go to");
    while (1) {
        editor_refresh_screen();
        editor_process_key();
    }
    return 0;
}
