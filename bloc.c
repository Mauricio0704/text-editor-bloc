#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <string.h>

#define CTRL_KEY(k) ((k) & 0x1f)

struct editorConfig {
    int screenrows;
    int screencols;
    struct termios orig_terminal_config;
};
struct editorConfig E;

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

char editor_read_key() {
    char c;
    int nstatus;
    while ((nstatus = read(STDIN_FILENO, &c, 1)) != 1)
        if (nstatus == -1 && errno != EAGAIN)
            die("editor read key");
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

void editor_draw_rows() {
  for (int y = 0; y < E.screenrows - 1; y++) {
    write(STDOUT_FILENO, "$\r\n", 3);
  }
  write(STDOUT_FILENO, "*", 1);
}

void editor_refresh_screen() {
    write(STDOUT_FILENO, "\x1b[2J", 4); // Escape secuence to clear all the screen
    write(STDOUT_FILENO, "\x1b[H", 3);

    editor_draw_rows();
    write(STDOUT_FILENO, "\x1b[H", 3);
}

void editor_process_key() {
    char c = editor_read_key();

    switch (c) {
        case CTRL_KEY('e'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(EXIT_SUCCESS);
            break;
        default:
            printf("%d: %c\r\n", c, c);
            break;
    }
}

void init_editor() {
  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main() {
    enableRawMode();
    init_editor();

    while (1) {
        editor_refresh_screen();
        editor_process_key();
    }
    return 0;
}