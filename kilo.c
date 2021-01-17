/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY, 
    END_KEY,
    PAGE_UP, 
    PAGE_DOWN
};

/*** data ***/

/* Editor row */
typedef struct erow {
    int size;
    char *chars;
} erow;

struct editorConfig {
    int cx, cy;
    int screenrows;
    int screencols;
    int numrows;
    erow row;
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
 * 
 *  Return:
 *      (int): If valid key, editor key that was read. Else, '\x1b'
 */
int editorReadKey() {
    int nread; 
    char c;
    while ((nread = read(STDERR_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            die("read");
        }
    }

    // Handle escape character
    if (c== '\x1b') {
        char seq[3];

        // Read two bytes
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        // Check for known  escape sequence
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                // Read a third character
                if (read(STDERR_FILENO, &seq[2], 1) != 1) {
                    return '\x1b';
                }
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }

            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        // If unknown escape sequence, return <esc>
        return '\x1b';
    } else {
        return c;
    }
}

/**
 *  getCursorPosition:
 *  Gets the current position of the cursor.
 *      
 *  Args:
 *      (int *) rows: Pointer to the int to store row position
 *      (int *) cols: Pointer to the int to store col position
 * 
 *  Return:
 *      (int): 0 if success, -1 if an error occurs
 */
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
 * 
 *  Return:
 *      (int): 0 if success, -1 if an error occurs
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

/*** file i/o ***/

/**
 *  editorOpen:
 */
void editorOpen(char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        die("fopen");
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    linelen = getline(&line, &linecap, fp);
    if (linelen != -1) {
        while (linelen > 0 && (line[linelen-1] == '\n' || line[linelen-1] == '\r')) {
            linelen--;
        }
        // Copy line into E.row.chars
        E.row.size = linelen;
        E.row.chars = malloc(linelen + 1);
        memcpy(E.row.chars, line, linelen);
        E.row.chars[linelen] = '\0';
        E.numrows = 1;
    }
    free(line);
    fclose(fp);
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
 * 
 *  Args:
 *      (struct abuf *) ab: Pointer to the current buffer
 *      (const char *) s: The string to add to the buffer
 *      (int) len: The length of s
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
 * 
 *  Args:
 *      (struct abuf *) ab: Pointer to the current buffer
 */
void abFree(struct abuf *ab) {
    free(ab->b);
}

/*** output ***/

/**
 * editorDrawRows:
 * Draw the rows of the editor into the buffer.
 * 
 *  Args:
 *      (struct abuf *) ab: Pointer to the current buffer
 */
void editorDrawRows(struct abuf *ab) {
    int y;
    for (y = 0 ; y < E.screenrows; y++) {
        if (y >= E.numrows) {
            // Print row that comes after the end of the text buffer
            if (E.numrows == 0 && y == E.screenrows / 3) {
                // Display welcome message
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);
                if (welcomelen > E.screencols) {
                    welcomelen = E.screencols;
                }

                // Center the message
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) {
                    abAppend(ab, " ", 1);
                }
                abAppend(ab, welcome, welcomelen);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            // Print row that is part of the text buffer
            int len = E.row.size;
            if (len > E.screencols) {
                len = E.screencols;
            }
            abAppend(ab, E.row.chars, len);
        }

        abAppend(ab, "\x1b[K", 3); // Clear the line before redraw
        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

/**
 * editorRefreshScreen:
 * Clears the terminal.
 */
void editorRefreshScreen() {
    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);  // Hide the cursor
    abAppend(&ab, "\x1b[H", 3);     // Reposition the cursor to top-left

    editorDrawRows(&ab);

    // Move cursor to position given by E
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);  // Show the cursor

    // Write the buffer to the terminal
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input ***/

/**
 * editorMoveCursor:
 * Moves the cursor given an input key.
 * 
 * Args:
 *      (int) key: The code for the key that was pressed
 */
void editorMoveCursor(int key) {
    switch (key) {
        case ARROW_LEFT:
            if (E.cx > 0) {
                E.cx--;
            }
            break;
        case ARROW_RIGHT:
            if (E.cx < E.screencols - 1) {
                E.cx++;
            }
            break;
        case ARROW_UP:
            if (E.cy > 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy < E.screenrows - 1) {
                E.cy++;
            }
            break;
    }
}

/**
 *  editorProcessKeypress:
 *  Waits for a keypress and handles that keypress.
 */
void editorProcessKeypress() {
    int c = editorReadKey();
    int temp;

    switch (c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            E.cx = E.screencols - 1;
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            temp = E.screenrows;
            while (temp--) {
                editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
    } 

}


/*** init ***/

/**
 *  initEditor:
 *  Initializes the editor configurations.
 */
void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.numrows = 0;

    int rc = getWindowSize(&E.screenrows, &E.screencols);
    if (rc == -1) {
        die("getWindowSize");
    }
}

/**
 * main:
 * Main function to the program.
 */
int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}
