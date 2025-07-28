#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>

#define CTRL_KEY(k) ((k) & 0x1f)

typedef struct erow {
    int size;
    char *chars;
} erow;

enum editor_key {
    ARROW_LEFT = 1000,
    ARROW_UP = 1001,
    ARROW_RIGHT = 1002,
    ARROW_DOWN = 1003
};

struct editorConfig {
    int cx, cy;
    int screenrows;
    int screencols;
    int numrows;
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
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        return -1;
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

void editor_append_row(char *line, int linelen) {
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    int end = E.numrows;
    E.row[end].size = linelen;
    E.row[end].chars = malloc(linelen + 1);
    memcpy(E.row[end].chars, line, linelen);
    E.row[end].chars[linelen] = '\0';
    E.numrows++;
}

void editorOpen(char *filename) {
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

void editor_draw_rows(struct abuf *wbatch) {
    abAppend(wbatch, "\r\n", 2);
    for (int y = 1; y < E.screenrows - 1; y++) {
        if (y > E.numrows) {
            if (E.numrows == 0 && y == 5) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "$ Bloc editor\r\n");
                abAppend(wbatch, welcome, welcomelen);
            } else
                abAppend(wbatch, "$\r\n", 3);
            abAppend(wbatch, "\x1b[K", 3); // Erase the right part of the line
        } else {
            int len = E.row[y-1].size;
            if (len > E.screencols) 
                len = E.screencols;
            abAppend(wbatch, "$ ", 2);
            abAppend(wbatch, E.row[y-1].chars, len);
            abAppend(wbatch, "\r\n", 2);
            abAppend(wbatch, "\x1b[K", 3);
        }
    }
    abAppend(wbatch, "*", 1);
    abAppend(wbatch, "\x1b[K", 3);
}

void editor_refresh_screen() {
    struct abuf wbatch = {NULL, 0}; 

    abAppend(&wbatch, "\x1b[?25l", 6); // Hide cursor
    abAppend(&wbatch, "\x1b[H", 3); // Position the cursor at the start of the screen

    editor_draw_rows(&wbatch);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&wbatch, buf, strlen(buf));

    abAppend(&wbatch, "\x1b[?25h", 6); // Show cursor

    write(STDOUT_FILENO, wbatch.buf, wbatch.len); // Write everything at once
    abFree(&wbatch);
}

void editorMoveCursor(int key) {
    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0)
                E.cx--;
            break;
        case ARROW_RIGHT:
            if (E.cx != E.screencols - 1)
                E.cx++;
            break;
        case ARROW_UP:
            if (E.cy != 0)
                E.cy--;
            break;
        case ARROW_DOWN:
            if (E.cy != E.screenrows - 1)
                E.cy++;
            break;
    }
}

void editor_process_key() {
    int c = editor_read_key();

    switch (c) {
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
            break;
    }
}

void init_editor() {
    E.cx = 2;
    E.cy = 1;
    E.numrows = 0;
    E.row = NULL;
    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
}

int main(int argc, char *argv[]) {
    enableRawMode();
    init_editor();
    
    if (argc >= 2)
        editorOpen(argv[1]);

    while (1) {
        editor_refresh_screen();
        editor_process_key();
    }
    return 0;
}