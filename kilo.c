/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)

/*** data ***/

struct editorConfig {
    int screenrows;
    int screencols;
    struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/

/**
 *  die:
 *  Print an error message and exits the program
 *      
 *  Args:
 *      (char *) s: The string to be displayed to the console 
 */
void die(const char *s) {
    // Clear the screen and reposition the cursor
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    // Print error messsage to screen
    perror(s);

    exit(1);
}

/**
 *  disableRawMode
 *  Sets terminal attributes to the original attributes
 */
void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
        die("tcsetattr");
    }
}

/**
 * enableRawMode
 * Updates terminal attributes and enters raw mode
 */
void enableRawMode() {
    // Get the initial terminal attributes
    int getRes = tcgetattr(STDIN_FILENO, &E.orig_termios);   
    if (getRes == -1) {
        die("tcgetattr");
    }
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;

    // Set terminal input flags
    // -- IXON: Disables software flow control (Ctrl-S and Ctrl-Q)
    // -- ICRNL: Don't turn carriage return CR into newline Nl
    // -- Misc.
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON );

    // Set teminal output flags
    // -- OPOST: Turn off all output processing features
    raw.c_oflag &= ~(OPOST);

    // Misc. 
    raw.c_cflag |= (CS8);

    // Set terminal local flags:
    // -- ECHO: Do not print keyboard inputs to stdout
    // -- ICANON: Turn off canonical mode 
    // -- ISIG: Disable SIGINT (Ctrl-C) and SIGSTP (Ctrl-Z) 
    // -- IEXTEN: Disable Ctrl-V for sending char literals
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    // VMIN: min number of bytes of input needed before read() can return
    raw.c_cc[VMIN] = 0;
    // VTIME: max amount of time to wait before read() returns
    raw.c_cc[VTIME] = 1;

    // Set the new terminal attributes
    int setRes = tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    if (setRes == -1) {
        die("tcsetattr");
    }
}

/**
 *  editorReadKey
 *  Waits for a single keypress and returns that keypress
 */
char editorReadKey() {
    int nread; 
    char c;
    while ((nread = read(STDERR_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            die("read");
        }
    }
    return c;
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    // Ask terminal for cursor position
    int rc = write(STDOUT_FILENO, "\x1b[6n", 4);
    if (rc != 4) return -1;

    // Read the reply from stdin
    while (i < sizeof(buf) - 1) {
        int rc = read(STDIN_FILENO, &buf[i], 1);
        if (rc != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    // Parse the input string for cursor position
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;  

    return 0;
}

/**
 *  getWindowSize:
 *  Gets the window size of the terminal
 * 
 *  Args:
 *      (int *) rows: pointer to int to store the number of rows in terminal
 *      (int *) cols: pointer to int to store the number of columns in terminal
 */
int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    // Use ioctl to get terminal dimensions
    int rc = ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    if (1 || rc == -1 || ws.ws_col == 0) {
        // Fallback if ioctl fails by moving cursor to bottom right
        rc = write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12);
        if (rc != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** append buffer ***/

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

/**
 *  abAppend:
 *  Appends a string the existing buffer.
 */
void abAppend(struct abuf *ab, const char *s, int len) {
    // Allocate enough memory to hold the new string
    char *new = realloc(ab->b, ab->len + len);

    // Copy new string into newly allocated memory
    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

/**
 *  abFree:
 *  Frees the memory used for buffer.
 */
void abFree(struct abuf *ab) {
    free(ab->b);
}

/*** output ***/

/**
 * editorDrawRows:
 */
void editorDrawRows(struct abuf *ab) {
    int y;
    for (y = 0 ; y < E.screenrows; y++) {
        abAppend(ab, "~", 1);

        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

/**
 * editorRefreshScreen:
 * Clears the terminal screen.
 */
void editorRefreshScreen() {
    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[2J", 4);    // Clear the entire screen
    abAppend(&ab, "\x1b[H", 3);     // Reposition the cursor to top-left

    editorDrawRows(&ab);

    abAppend(&ab, "\x1b[H", 3);     // Reposition the cursor to top-left

    // Write the buffer to the terminal
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input ***/

/**
 *  editorProcessKeypress:
 *  Waits for a keypress and handles that keypress.
 */
void editorProcessKeypress() {
    char c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
    }   

}


/*** init ***/

void initEditor() {
    int rc = getWindowSize(&E.screenrows, &E.screencols);
    if (rc == -1) {
        die("getWindowSize");
    }
}

int main() {
    enableRawMode();
    initEditor();

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}
