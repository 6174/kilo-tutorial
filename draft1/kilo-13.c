#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct erow {
  int size;
  char *chars;
} erow;

struct editorConfig {
  int cx, cy;
  int screenrows;
  int screencols;
  int rawmode;
  int numrows;
  erow *row;
};

static struct editorConfig E;

enum KEY_ACTION {
  CTRL_Q = 17,
  ESC = 27,

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

void fatal(const char *s) {
  perror(s);
  exit(1);
}

static struct termios orig_termios;

void disableRawMode() {
  if (E.rawmode) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    E.rawmode = 0;
  }
}

void enableRawMode() {
  struct termios raw;

  if (E.rawmode) return;
  if (!isatty(STDIN_FILENO)) { errno = ENOTTY; fatal("enableRawMode"); }
  atexit(disableRawMode);
  if (tcgetattr(STDIN_FILENO, &orig_termios) < 0) fatal("tcgetattr");

  raw = orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) fatal("tcsetattr");
  E.rawmode = 1;
}

int editorReadKey() {
  int nread;
  char c, seq[3];
  while ((nread = read(STDIN_FILENO, &c, 1)) == 0);
  if (nread == -1) fatal("read");

  while (1) {
    switch (c) {
      case ESC:
        if (read(STDIN_FILENO, seq, 1) == 0) return ESC;
        if (read(STDIN_FILENO, seq + 1, 1) == 0) return ESC;

        if (seq[0] == '[') {
          if (seq[1] >= '0' && seq[1] <= '9') {
            if (read(STDIN_FILENO, seq + 2, 1) == 0) return ESC;
            if (seq[2] == '~') {
              switch (seq[1]) {
                case '3': return DEL_KEY;
                case '5': return PAGE_UP;
                case '6': return PAGE_DOWN;
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
        break;

      default:
        return c;
    }
  }
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, buf + i, 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';

  if (buf[0] != ESC || buf[1] != '[') return -1;
  if (sscanf(buf + 2, "%d;%d", rows, cols) != 2) return -1;
  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    int orig_row, orig_col, retval;

    retval = getCursorPosition(&orig_row, &orig_col);
    if (retval == -1) return -1;

    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    retval = getCursorPosition(rows, cols);
    if (retval == -1) return -1;

    char seq[32];
    snprintf(seq, 32, "\x1b[%d;%dH", orig_row, orig_col);
    if (write(STDOUT_FILENO, seq, strlen(seq)) == -1) {
      /* Can't recover... */
    }
    return 0;
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

void editorInsertRow(int at, char *s, size_t len) {
  if (at > E.numrows) return;
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  if (at != E.numrows) {
    memmove(E.row + at + 1, E.row + at, sizeof(E.row[0]) * (E.numrows - at));
  }
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len + 1);
  E.numrows++;
}

void editorOpen(char *filename) {
  FILE *fp;

  fp = fopen(filename, "r");
  if (!fp) fatal("Opening file");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    if (linelen && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      line[--linelen] = '\0';
    editorInsertRow(E.numrows, line, linelen);
  }
  free(line);
  fclose(fp);
}

struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) return;
  memcpy(new + ab->len, s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
}

void editorRefreshScreen() {
  int y;
  erow *r;
  char buf[32];
  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[H", 3);
  for (y = 0; y < E.screenrows; y++) {
    if (y >= E.numrows) {
      abAppend(&ab, "~\x1b[0K", 5);
      if (y < E.screenrows - 1) {
        abAppend(&ab, "\r\n", 2);
      }
      continue;
    }

    r = &E.row[y];

    int len = r->size;
    if (len > 0) {
      if (len > E.screencols) len = E.screencols;
      abAppend(&ab, r->chars, len);
    }
    abAppend(&ab, "\x1b[0K", 4);
    if (y < E.screenrows - 1) {
      abAppend(&ab, "\r\n", 2);
    }
  }
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
  abAppend(&ab, buf, strlen(buf));
  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void editorMoveCursor(int key) {
  int filerow = E.cy;
  int filecol = E.cx;
  int rowlen;
  erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

  switch (key) {
    case ARROW_LEFT:
      if (E.cx == 0) {
        if (filerow > 0) {
          E.cy--;
          E.cx = E.row[filerow - 1].size;
          if (E.cx > E.screencols - 1) {
            E.cx = E.screencols - 1;
          }
        }
      } else {
        E.cx -= 1;
      }
      break;
    case ARROW_RIGHT:
      if (row && filecol < row->size) {
        if (E.cx != E.screencols - 1) {
          E.cx += 1;
        }
      } else if (row && filecol == row->size) {
        E.cx = 0;
        if (E.cy != E.screenrows - 1) {
          E.cy += 1;
        }
      }
      break;
    case ARROW_UP:
      if (E.cy > 0) {
        E.cy -= 1;
      }
      break;
    case ARROW_DOWN:
      if (filerow < E.numrows) {
        if (E.cy != E.screenrows - 1) {
          E.cy += 1;
        }
      }
      break;
  }

  filerow = E.cy;
  filecol = E.cx;
  row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
  rowlen = row ? row->size : 0;
  if (filecol > rowlen) {
    E.cx -= filecol - rowlen;
    if (E.cx < 0) {
      E.cx = 0;
    }
  }
}

void editorProcessKeypress() {
  int c = editorReadKey();

  switch (c) {
    case CTRL_Q:
      exit(0);
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;
  }
}

void initEditor() {
  E.cx = 0;
  E.cy = 0;
  E.rawmode = 0;
  E.numrows = 0;
  E.row = NULL;
  if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
    fatal("Unable to query the screen for size (columns / rows)");
  }
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: kilo <filename>\n");
    exit(1);
  }

  initEditor();
  editorOpen(argv[1]);
  enableRawMode();

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}

