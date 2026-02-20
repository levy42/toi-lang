/* pua_lineedit.c -- minimalist local line editing for the Pua REPL.
 * line editing lib needs to be 20,000 lines of C code.
 *
 * Derived from linenoise and adapted for this codebase.
 *
 * ------------------------------------------------------------------------
 *
 * Copyright (c) 2010-2023, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ------------------------------------------------------------------------
 *
 * References:
 * - http://invisible-island.net/xterm/ctlseqs/ctlseqs.html
 * - http://www.3waylabs.com/nw/WWW/products/wizcon/vt220.html
 *
 * Todo list:
 * - Filter bogus Ctrl+<char> combinations.
 * - Win32 support
 *
 * Bloat:
 * - History search like Ctrl+r in readline?
 *
 * List of escape sequences used by this program, we do everything just
 * with three sequences. In order to be so cheap we may have some
 * flickering effect with some slow terminal, but the lesser sequences
 * the more compatible.
 *
 * EL (Erase Line)
 *    Sequence: ESC [ n K
 *    Effect: if n is 0 or missing, clear from cursor to end of line
 *    Effect: if n is 1, clear from beginning of line to cursor
 *    Effect: if n is 2, clear entire line
 *
 * CUF (CUrsor Forward)
 *    Sequence: ESC [ n C
 *    Effect: moves cursor forward n chars
 *
 * CUB (CUrsor Backward)
 *    Sequence: ESC [ n D
 *    Effect: moves cursor backward n chars
 *
 * The following is used to get the terminal width if getting
 * the width with the TIOCGWINSZ ioctl fails
 *
 * DSR (Device Status Report)
 *    Sequence: ESC [ 6 n
 *    Effect: reports the current cusor position as ESC [ n ; m R
 *            where n is the row and m is the column
 *
 * When multi line mode is enabled, we also use an additional escape
 * sequence. However multi line editing is disabled by default.
 *
 * CUU (Cursor Up)
 *    Sequence: ESC [ n A
 *    Effect: moves cursor up of n chars.
 *
 * CUD (Cursor Down)
 *    Sequence: ESC [ n B
 *    Effect: moves cursor down of n chars.
 *
 * When clearing the screen (Ctrl+L), two additional escape sequences
 * are used in order to clear the screen and position the cursor at home
 * position.
 *
 * CUP (Cursor position)
 *    Sequence: ESC [ H
 *    Effect: moves the cursor to upper left corner
 *
 * ED (Erase display)
 *    Sequence: ESC [ 2 J
 *    Effect: clear the whole screen
 *
 */

#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include "pua_lineedit.h"

#define PUA_LINEEDIT_DEFAULT_HISTORY_MAX_LEN 100
#define PUA_LINEEDIT_MAX_LINE 4096
static char *unsupported_term[] = {"dumb","cons25","emacs",NULL};
static pua_lineedit_completion_callback *completion_callback = NULL;
static pua_lineedit_syntax_highlight_callback *syntax_highlight_callback = NULL;
struct pua_lineedit_state;
static char *pua_lineedit_no_tty(void);
static void refresh_line_with_completion(struct pua_lineedit_state *ls, pua_lineedit_completions *lc, int flags);
static void refresh_line_with_flags(struct pua_lineedit_state *l, int flags);

static struct termios orig_termios; /* In order to restore at exit.*/
static int raw_mode = 0; /* For atexit() function to check if restore is needed*/
static int ml_mode = 0;  /* Multi line mode. Default is single line. */
static int atexit_registered = 0; /* Register atexit just 1 time. */
static int history_max_len = PUA_LINEEDIT_DEFAULT_HISTORY_MAX_LEN;
static int history_len = 0;
static char **history = NULL;

/* Internal editor state. Kept private since we only expose the blocking API. */
struct pua_lineedit_state {
    int in_completion;
    size_t completion_idx;
    int ifd;
    int ofd;
    char *buf;
    size_t buflen;
    const char *prompt;
    size_t plen;
    size_t pos;
    size_t oldpos;
    size_t len;
    size_t cols;
    size_t oldrows;
    int oldrpos;
    int history_index;
};

/* =========================== Character handling =========================== */

/* Byte-based character movement. This is intentionally simpler than full
 * UTF-8 grapheme handling. */
static size_t prev_char_len(const char *buf, size_t pos) {
    (void)buf;
    return pos > 0 ? 1 : 0;
}

static size_t next_char_len(const char *buf, size_t pos, size_t len) {
    (void)buf;
    return pos < len ? 1 : 0;
}

/* Display width approximation: treat one byte as one column. */
static size_t str_width(const char *s, size_t len) {
    (void)s;
    return len;
}

enum KEY_ACTION{
	KEY_NULL = 0,	    /* NULL */
	CTRL_A = 1,         /* Ctrl+a */
	CTRL_B = 2,         /* Ctrl-b */
	CTRL_C = 3,         /* Ctrl-c */
	CTRL_D = 4,         /* Ctrl-d */
	CTRL_E = 5,         /* Ctrl-e */
	CTRL_F = 6,         /* Ctrl-f */
	CTRL_H = 8,         /* Ctrl-h */
	TAB = 9,            /* Tab */
	CTRL_K = 11,        /* Ctrl+k */
	CTRL_L = 12,        /* Ctrl+l */
	ENTER = 13,         /* Enter */
	CTRL_N = 14,        /* Ctrl-n */
	CTRL_P = 16,        /* Ctrl-p */
	CTRL_T = 20,        /* Ctrl-t */
	CTRL_U = 21,        /* Ctrl+u */
	CTRL_W = 23,        /* Ctrl+w */
	ESC = 27,           /* Escape */
	BACKSPACE =  127    /* Backspace */
};

static void pua_lineedit_at_exit(void);
int pua_lineedit_history_add(const char *line);
#define REFRESH_CLEAN (1<<0)    // Clean the old prompt from the screen
#define REFRESH_WRITE (1<<1)    // Rewrite the prompt on the screen.
#define REFRESH_ALL (REFRESH_CLEAN|REFRESH_WRITE) // Do both.
static void refresh_line(struct pua_lineedit_state *l);

/* Debugging macro. */
#if 0
FILE *lndebug_fp = NULL;
#define lndebug(...) \
    do { \
        if (lndebug_fp == NULL) { \
            lndebug_fp = fopen("/tmp/lndebug.txt","a"); \
            fprintf(lndebug_fp, \
            "[%d %d %d] p: %d, rows: %d, rpos: %d, max: %d, oldmax: %d\n", \
            (int)l->len,(int)l->pos,(int)l->oldpos,plen,rows,rpos, \
            (int)l->oldrows,old_rows); \
        } \
        fprintf(lndebug_fp, ", " __VA_ARGS__); \
        fflush(lndebug_fp); \
    } while (0)
#else
#define lndebug(fmt, ...)
#endif

/* ======================= Low level terminal handling ====================== */

/* Set if to use or not the multi line mode. */
void pua_lineedit_set_multi_line(int ml) {
    ml_mode = ml;
}

/* Return true if the terminal name is in the list of terminals we know are
 * not able to understand basic escape sequences. */
static int is_unsupported_term(void) {
    char *term = getenv("TERM");
    int j;

    if (term == NULL) return 0;
    for (j = 0; unsupported_term[j]; j++)
        if (!strcasecmp(term,unsupported_term[j])) return 1;
    return 0;
}

/* Raw mode: 1960 magic shit. */
static int enable_raw_mode(int fd) {
    struct termios raw;

    /* Test mode: when LINENOISE_ASSUME_TTY is set, skip terminal setup.
     * This allows testing via pipes without a real terminal. */
    if (getenv("LINENOISE_ASSUME_TTY")) {
        raw_mode = 1;
        return 0;
    }

    if (!isatty(STDIN_FILENO)) goto fatal;
    if (!atexit_registered) {
        atexit(pua_lineedit_at_exit);
        atexit_registered = 1;
    }
    if (tcgetattr(fd,&orig_termios) == -1) goto fatal;

    raw = orig_termios;  /* modify the original mode */
    /* input modes: no break, no CR to NL, no parity check, no strip char,
     * no start/stop output control. */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    /* output modes - disable post processing */
    raw.c_oflag &= ~(OPOST);
    /* control modes - set 8 bit chars */
    raw.c_cflag |= (CS8);
    /* local modes - choing off, canonical off, no extended functions,
     * no signal chars (^Z,^C) */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    /* control chars - set return condition: min number of bytes and timer.
     * We want read to return every single byte, without timeout. */
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0; /* 1 byte, no timer */

    /* put terminal in raw mode after flushing */
    if (tcsetattr(fd,TCSAFLUSH,&raw) < 0) goto fatal;
    raw_mode = 1;
    return 0;

fatal:
    errno = ENOTTY;
    return -1;
}

static void disable_raw_mode(int fd) {
    /* Test mode: nothing to restore. */
    if (getenv("LINENOISE_ASSUME_TTY")) {
        raw_mode = 0;
        return;
    }
    /* Don't even check the return value as it's too late. */
    if (raw_mode && tcsetattr(fd,TCSAFLUSH,&orig_termios) != -1)
        raw_mode = 0;
}

/* Try to get the number of columns in the current terminal, or assume 80
 * if it fails. */
static int get_columns(int ifd, int ofd) {
    struct winsize ws;
    (void)ifd;
    (void)ofd;

    /* Test mode: use LINENOISE_COLS env var for fixed width. */
    char *cols_env = getenv("LINENOISE_COLS");
    if (cols_env) return atoi(cols_env);

    if (ioctl(1, TIOCGWINSZ, &ws) != -1 && ws.ws_col > 0) return ws.ws_col;
    return 80;
}

/* Clear the screen. Used to handle ctrl+l */
static void clear_screen(void) {
    if (write(STDOUT_FILENO,"\x1b[H\x1b[2J",7) <= 0) {
        /* nothing to do, just to avoid warning. */
    }
}

/* Beep, used for completion when there is nothing to complete or when all
 * the choices were already shown. */
static void pua_lineedit_beep(void) {
    fprintf(stderr, "\x7");
    fflush(stderr);
}

/* ============================== Completion ================================ */

/* Free a list of completion option populated by pua_lineedit_add_completion(). */
static void free_completions(pua_lineedit_completions *lc) {
    size_t i;
    for (i = 0; i < lc->len; i++)
        free(lc->cvec[i]);
    if (lc->cvec != NULL)
        free(lc->cvec);
}

/* Called by complete_line() to render the current
 * edited line with the proposed completion. If the current completion table
 * is already available, it is passed as second argument, otherwise the
 * function will use the callback to obtain it.
 *
 * Flags are the same as refresh_line*(), that is REFRESH_* macros. */
static void refresh_line_with_completion(struct pua_lineedit_state *ls, pua_lineedit_completions *lc, int flags) {
    /* Obtain the table of completions if the caller didn't provide one. */
    pua_lineedit_completions ctable = { 0, NULL };
    if (lc == NULL) {
        completion_callback(ls->buf,&ctable);
        lc = &ctable;
    }

    /* Show the edited line with completion if possible, or just refresh. */
    if (ls->completion_idx < lc->len) {
        struct pua_lineedit_state saved = *ls;
        ls->len = ls->pos = strlen(lc->cvec[ls->completion_idx]);
        ls->buf = lc->cvec[ls->completion_idx];
        refresh_line_with_flags(ls,flags);
        ls->len = saved.len;
        ls->pos = saved.pos;
        ls->buf = saved.buf;
    } else {
        refresh_line_with_flags(ls,flags);
    }

    /* Free the completions table if needed. */
    if (lc != &ctable) free_completions(&ctable);
}

/* This helper is called when the user
 * user types the <tab> key in order to complete the string currently in the
 * input.
 *
 * The state of the editing is encapsulated into the pointed pua_lineedit_state
 * structure as described in the structure definition.
 *
 * If the function returns non-zero, the caller should handle the
 * returned value as a byte read from the standard input, and process
 * it as usually: this basically means that the function may return a byte
 * read from the termianl but not processed. Otherwise, if zero is returned,
 * the input was consumed by the complete_line() function to navigate the
 * possible completions, and the caller should read for the next characters
 * from stdin. */
static int complete_line(struct pua_lineedit_state *ls, int keypressed) {
    pua_lineedit_completions lc = { 0, NULL };
    int nwritten;
    char c = keypressed;

    completion_callback(ls->buf,&lc);
    if (lc.len == 0) {
        pua_lineedit_beep();
        ls->in_completion = 0;
    } else {
        switch(c) {
            case 9: /* tab */
                if (ls->in_completion == 0) {
                    ls->in_completion = 1;
                    ls->completion_idx = 0;
                } else {
                    ls->completion_idx = (ls->completion_idx+1) % (lc.len+1);
                    if (ls->completion_idx == lc.len) pua_lineedit_beep();
                }
                c = 0;
                break;
            case 27: /* escape */
                /* Re-show original buffer */
                if (ls->completion_idx < lc.len) refresh_line(ls);
                ls->in_completion = 0;
                c = 0;
                break;
            default:
                /* Update buffer and return */
                if (ls->completion_idx < lc.len) {
                    nwritten = snprintf(ls->buf,ls->buflen,"%s",
                        lc.cvec[ls->completion_idx]);
                    ls->len = ls->pos = nwritten;
                }
                ls->in_completion = 0;
                break;
        }

        /* Show completion or original buffer */
        if (ls->in_completion && ls->completion_idx < lc.len) {
            refresh_line_with_completion(ls,&lc,REFRESH_ALL);
        } else {
            refresh_line(ls);
        }
    }

    free_completions(&lc);
    return c; /* Return last read character */
}

/* Register a callback function to be called for tab-completion. */
void pua_lineedit_set_completion_callback(pua_lineedit_completion_callback *fn) {
    completion_callback = fn;
}

void pua_lineedit_set_syntax_highlight_callback(pua_lineedit_syntax_highlight_callback *fn) {
    syntax_highlight_callback = fn;
}

/* This function is used by the callback function registered by the user
 * in order to add completion options given the input string when the
 * user typed <tab>. See the example.c source code for a very easy to
 * understand example. */
void pua_lineedit_add_completion(pua_lineedit_completions *lc, const char *str) {
    size_t len = strlen(str);
    char *copy, **cvec;

    copy = malloc(len+1);
    if (copy == NULL) return;
    memcpy(copy,str,len+1);
    cvec = realloc(lc->cvec,sizeof(char*)*(lc->len+1));
    if (cvec == NULL) {
        free(copy);
        return;
    }
    lc->cvec = cvec;
    lc->cvec[lc->len++] = copy;
}

/* =========================== Line editing ================================= */

/* We define a very simple "append buffer" structure, that is an heap
 * allocated string where we can append to. This is useful in order to
 * write all the escape sequences in a buffer and flush them to the standard
 * output in a single call, to avoid flickering effects. */
struct abuf {
    char *b;
    int len;
};

static void ab_init(struct abuf *ab) {
    ab->b = NULL;
    ab->len = 0;
}

static void ab_append(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b,ab->len+len);

    if (new == NULL) return;
    memcpy(new+ab->len,s,len);
    ab->b = new;
    ab->len += len;
}

static void ab_free(struct abuf *ab) {
    free(ab->b);
}

/* Single line low level line refresh. */
static void refresh_single_line(struct pua_lineedit_state *l, int flags) {
    char seq[64];
    size_t pwidth = str_width(l->prompt, l->plen); /* Prompt display width */
    int fd = l->ofd;
    char *buf = l->buf;
    size_t len = l->len;
    size_t pos = l->pos;
    size_t poscol;
    size_t lencol;
    struct abuf ab;

    /* Calculate display width up to cursor and total display width. */
    poscol = str_width(buf, pos);
    lencol = str_width(buf, len);

    /* Scroll the buffer horizontally if cursor is past the right edge. */
    while (pwidth + poscol >= l->cols) {
        size_t clen = next_char_len(buf, 0, len);
        buf += clen;
        len -= clen;
        pos -= clen;
        poscol -= 1;
        lencol -= 1;
    }

    /* Trim from the right if the line still doesn't fit. */
    while (pwidth + lencol > l->cols) {
        size_t clen = prev_char_len(buf, len);
        len -= clen;
        lencol -= 1;
    }

    ab_init(&ab);
    /* Cursor to left edge */
    snprintf(seq,sizeof(seq),"\r");
    ab_append(&ab,seq,strlen(seq));

    if (flags & REFRESH_WRITE) {
        /* Write the prompt and the current buffer content */
        ab_append(&ab,l->prompt,l->plen);
        if (syntax_highlight_callback) {
            char highlighted[4096];
            syntax_highlight_callback(buf, highlighted, sizeof(highlighted));
            ab_append(&ab, highlighted, strlen(highlighted));
        } else {
            ab_append(&ab,buf,len);
        }
    }

    /* Erase to right */
    snprintf(seq,sizeof(seq),"\x1b[0K");
    ab_append(&ab,seq,strlen(seq));

    if (flags & REFRESH_WRITE) {
        /* Move cursor to original position. */
        snprintf(seq,sizeof(seq),"\r\x1b[%dC", (int)(poscol+pwidth));
        ab_append(&ab,seq,strlen(seq));
    }

    if (write(fd,ab.b,ab.len) == -1) {} /* Can't recover from write error. */
    ab_free(&ab);
}

/* Multi line low level line refresh. */
static void refresh_multi_line(struct pua_lineedit_state *l, int flags) {
    char seq[64];
    size_t pwidth = str_width(l->prompt, l->plen);  /* Prompt display width */
    size_t bufwidth = str_width(l->buf, l->len);    /* Buffer display width */
    size_t poswidth = str_width(l->buf, l->pos);    /* Cursor display width */
    int rows = (pwidth+bufwidth+l->cols-1)/l->cols;    /* rows used by current buf. */
    int rpos = l->oldrpos;   /* cursor relative row from previous refresh. */
    int rpos2; /* rpos after refresh. */
    int col; /* column position, zero-based. */
    int old_rows = l->oldrows;
    int fd = l->ofd, j;
    struct abuf ab;

    l->oldrows = rows;

    /* First step: clear all the lines used before. To do so start by
     * going to the last row. */
    ab_init(&ab);

    if (flags & REFRESH_CLEAN) {
        if (old_rows-rpos > 0) {
            lndebug("go down %d", old_rows-rpos);
            snprintf(seq,64,"\x1b[%dB", old_rows-rpos);
            ab_append(&ab,seq,strlen(seq));
        }

        /* Now for every row clear it, go up. */
        for (j = 0; j < old_rows-1; j++) {
            lndebug("clear+up");
            snprintf(seq,64,"\r\x1b[0K\x1b[1A");
            ab_append(&ab,seq,strlen(seq));
        }
    }

    if (flags & REFRESH_ALL) {
        /* Clean the top line. */
        lndebug("clear");
        snprintf(seq,64,"\r\x1b[0K");
        ab_append(&ab,seq,strlen(seq));
    }

    if (flags & REFRESH_WRITE) {
        /* Write the prompt and the current buffer content */
        ab_append(&ab,l->prompt,l->plen);
        if (syntax_highlight_callback) {
            char highlighted[4096];
            syntax_highlight_callback(l->buf, highlighted, sizeof(highlighted));
            ab_append(&ab, highlighted, strlen(highlighted));
        } else {
            ab_append(&ab,l->buf,l->len);
        }

        /* If we are at the very end of the screen with our prompt, we need to
         * emit a newline and move the prompt to the first column. */
        if (l->pos &&
            l->pos == l->len &&
            (poswidth+pwidth) % l->cols == 0)
        {
            lndebug("<newline>");
            ab_append(&ab,"\n",1);
            snprintf(seq,64,"\r");
            ab_append(&ab,seq,strlen(seq));
            rows++;
            if (rows > (int)l->oldrows) l->oldrows = rows;
        }

        /* Move cursor to right position. */
        rpos2 = (pwidth+poswidth+l->cols)/l->cols; /* Current cursor relative row */
        lndebug("rpos2 %d", rpos2);

        /* Go up till we reach the expected position. */
        if (rows-rpos2 > 0) {
            lndebug("go-up %d", rows-rpos2);
            snprintf(seq,64,"\x1b[%dA", rows-rpos2);
            ab_append(&ab,seq,strlen(seq));
        }

        /* Set column. */
        col = (pwidth+poswidth) % l->cols;
        lndebug("set col %d", 1+col);
        if (col)
            snprintf(seq,64,"\r\x1b[%dC", col);
        else
            snprintf(seq,64,"\r");
        ab_append(&ab,seq,strlen(seq));
    }

    lndebug("\n");
    l->oldpos = l->pos;
    if (flags & REFRESH_WRITE) l->oldrpos = rpos2;

    if (write(fd,ab.b,ab.len) == -1) {} /* Can't recover from write error. */
    ab_free(&ab);
}

/* Calls the two low level functions refresh_single_line() or
 * refresh_multi_line() according to the selected mode. */
static void refresh_line_with_flags(struct pua_lineedit_state *l, int flags) {
    if (ml_mode)
        refresh_multi_line(l,flags);
    else
        refresh_single_line(l,flags);
}

/* Utility function to avoid specifying REFRESH_ALL all the times. */
static void refresh_line(struct pua_lineedit_state *l) {
    refresh_line_with_flags(l,REFRESH_ALL);
}

/* Insert the character(s) 'c' at cursor current position. */
static int pua_lineedit_edit_insert(struct pua_lineedit_state *l, const char *c, size_t clen) {
    if (l->len + clen <= l->buflen) {
        if (l->len == l->pos) {
            /* Append at end of line. */
            memcpy(l->buf+l->pos, c, clen);
            l->pos += clen;
            l->len += clen;
            l->buf[l->len] = '\0';
            if ((!ml_mode &&
                 str_width(l->prompt,l->plen)+str_width(l->buf,l->len) < l->cols)) {
                /* Avoid a full update of the line in the trivial case. */
                if (write(l->ofd,c,clen) == -1) return -1;
            } else {
                refresh_line(l);
            }
        } else {
            /* Insert in the middle of the line. */
            memmove(l->buf+l->pos+clen, l->buf+l->pos, l->len-l->pos);
            memcpy(l->buf+l->pos, c, clen);
            l->len += clen;
            l->pos += clen;
            l->buf[l->len] = '\0';
            refresh_line(l);
        }
    }
    return 0;
}

/* Move cursor left by one byte. */
static void pua_lineedit_edit_move_left(struct pua_lineedit_state *l) {
    if (l->pos > 0) {
        l->pos -= prev_char_len(l->buf, l->pos);
        refresh_line(l);
    }
}

/* Move cursor right by one byte. */
static void pua_lineedit_edit_move_right(struct pua_lineedit_state *l) {
    if (l->pos != l->len) {
        l->pos += next_char_len(l->buf, l->pos, l->len);
        refresh_line(l);
    }
}

/* Move cursor to the start of the line. */
static void pua_lineedit_edit_move_home(struct pua_lineedit_state *l) {
    if (l->pos != 0) {
        l->pos = 0;
        refresh_line(l);
    }
}

/* Move cursor to the end of the line. */
static void pua_lineedit_edit_move_end(struct pua_lineedit_state *l) {
    if (l->pos != l->len) {
        l->pos = l->len;
        refresh_line(l);
    }
}

/* Substitute the currently edited line with the next or previous history
 * entry as specified by 'dir'. */
#define PUA_LINEEDIT_HISTORY_NEXT 0
#define PUA_LINEEDIT_HISTORY_PREV 1
static void pua_lineedit_edit_history_next(struct pua_lineedit_state *l, int dir) {
    if (history_len > 1) {
        /* Update the current history entry before to
         * overwrite it with the next one. */
        free(history[history_len - 1 - l->history_index]);
        history[history_len - 1 - l->history_index] = strdup(l->buf);
        /* Show the new entry */
        l->history_index += (dir == PUA_LINEEDIT_HISTORY_PREV) ? 1 : -1;
        if (l->history_index < 0) {
            l->history_index = 0;
            return;
        } else if (l->history_index >= history_len) {
            l->history_index = history_len-1;
            return;
        }
        strncpy(l->buf,history[history_len - 1 - l->history_index],l->buflen);
        l->buf[l->buflen-1] = '\0';
        l->len = l->pos = strlen(l->buf);
        refresh_line(l);
    }
}

/* Delete the character at the right of the cursor. */
static void pua_lineedit_edit_delete(struct pua_lineedit_state *l) {
    if (l->len > 0 && l->pos < l->len) {
        size_t clen = next_char_len(l->buf, l->pos, l->len);
        memmove(l->buf+l->pos, l->buf+l->pos+clen, l->len-l->pos-clen);
        l->len -= clen;
        l->buf[l->len] = '\0';
        refresh_line(l);
    }
}

/* Backspace implementation. Deletes one byte before the cursor. */
static void pua_lineedit_edit_backspace(struct pua_lineedit_state *l) {
    if (l->pos > 0 && l->len > 0) {
        size_t clen = prev_char_len(l->buf, l->pos);
        memmove(l->buf+l->pos-clen, l->buf+l->pos, l->len-l->pos);
        l->pos -= clen;
        l->len -= clen;
        l->buf[l->len] = '\0';
        refresh_line(l);
    }
}

/* Delete the previous word, maintaining the cursor at the start of the word. */
static void pua_lineedit_edit_delete_prev_word(struct pua_lineedit_state *l) {
    size_t old_pos = l->pos;
    size_t diff;

    /* Skip spaces before the word. */
    while (l->pos > 0 && l->buf[l->pos-1] == ' ')
        l->pos -= prev_char_len(l->buf, l->pos);
    /* Skip non-space characters. */
    while (l->pos > 0 && l->buf[l->pos-1] != ' ')
        l->pos -= prev_char_len(l->buf, l->pos);
    diff = old_pos - l->pos;
    memmove(l->buf+l->pos, l->buf+old_pos, l->len-old_pos+1);
    l->len -= diff;
    refresh_line(l);
}

/* Internal helper used by the blocking API. It will:
 *
 * 1. Initialize the pua_lineedit state passed by the user.
 * 2. Put the terminal in RAW mode.
 * 3. Show the prompt.
 * 4. Return control so the caller can repeatedly call pua_lineedit_edit_feed().
 *
 * When pua_lineedit_edit_feed() returns non-NULL, the user finished with the
 * line editing session (pressed enter CTRL-D/C): in this case the caller
 * needs to call pua_lineedit_edit_stop() to put back the terminal in normal
 * mode. This will not destroy the buffer, as long as the pua_lineedit_state
 * is still valid in the context of the caller.
 *
 * The function returns 0 on success, or -1 if writing to standard output
 * fails. If stdin_fd or stdout_fd are set to -1, the default is to use
 * STDIN_FILENO and STDOUT_FILENO.
 */
static int pua_lineedit_edit_start(struct pua_lineedit_state *l, int stdin_fd, int stdout_fd, char *buf, size_t buflen, const char *prompt) {
    /* Populate the pua_lineedit state that we pass to functions implementing
     * specific editing functionalities. */
    l->in_completion = 0;
    l->ifd = stdin_fd != -1 ? stdin_fd : STDIN_FILENO;
    l->ofd = stdout_fd != -1 ? stdout_fd : STDOUT_FILENO;
    l->buf = buf;
    l->buflen = buflen;
    l->prompt = prompt;
    l->plen = strlen(prompt);
    l->oldpos = l->pos = 0;
    l->len = 0;

    /* Enter raw mode. */
    if (enable_raw_mode(l->ifd) == -1) return -1;

    l->cols = get_columns(stdin_fd, stdout_fd);
    l->oldrows = 0;
    l->oldrpos = 1;  /* Cursor starts on row 1. */
    l->history_index = 0;

    /* Buffer starts empty. */
    l->buf[0] = '\0';
    l->buflen--; /* Make sure there is always space for the nulterm */

    /* If stdin is not a tty, stop here with the initialization. We
     * will actually just read a line from standard input in blocking
     * mode later, in pua_lineedit_edit_feed(). */
    if (!isatty(l->ifd) && !getenv("LINENOISE_ASSUME_TTY")) return 0;

    /* The latest history entry is always our current buffer, that
     * initially is just an empty string. */
    pua_lineedit_history_add("");

    if (write(l->ofd,prompt,l->plen) == -1) return -1;
    return 0;
}

static char *pua_lineedit_edit_more = "pua_lineedit: internal edit-in-progress sentinel";

/* Internal helper for the blocking API. Called in a loop while reading from
 * standard input.
 *
 * The function returns pua_lineedit_edit_more to signal that line editing is still
 * in progress, that is, the user didn't yet pressed enter / CTRL-D. Otherwise
 * the function returns the pointer to the heap-allocated buffer with the
 * edited line, that the caller should free().
 *
 * On special conditions, NULL is returned and errno is populated:
 *
 * EAGAIN if the user pressed Ctrl-C
 * ENOENT if the user pressed Ctrl-D
 *
 * Some other errno: I/O error.
 */
static char *pua_lineedit_edit_feed(struct pua_lineedit_state *l) {
    /* Not a TTY, pass control to line reading without character
     * count limits. */
    if (!isatty(l->ifd) && !getenv("LINENOISE_ASSUME_TTY")) return pua_lineedit_no_tty();

    char c;
    int nread;
    char seq[3];

    nread = read(l->ifd,&c,1);
    if (nread < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? pua_lineedit_edit_more : NULL;
    } else if (nread == 0) {
        return NULL;
    }

    /* Only autocomplete when the callback is set. It returns < 0 when
     * there was an error reading from fd. Otherwise it will return the
     * character that should be handled next. */
    if ((l->in_completion || c == 9) && completion_callback != NULL) {
        c = complete_line(l,c);
        /* Return on errors */
        if (c < 0) return NULL;
        /* Read next character when 0 */
        if (c == 0) return pua_lineedit_edit_more;
    }

    switch(c) {
    case ENTER:    /* enter */
        history_len--;
        free(history[history_len]);
        if (ml_mode) pua_lineedit_edit_move_end(l);
        return strdup(l->buf);
    case CTRL_C:     /* ctrl-c */
        errno = EAGAIN;
        return NULL;
    case BACKSPACE:   /* backspace */
    case 8:     /* ctrl-h */
        pua_lineedit_edit_backspace(l);
        break;
    case CTRL_D:     /* ctrl-d, remove char at right of cursor, or if the
                        line is empty, act as end-of-file. */
        if (l->len > 0) {
            pua_lineedit_edit_delete(l);
        } else {
            history_len--;
            free(history[history_len]);
            errno = ENOENT;
            return NULL;
        }
        break;
    case CTRL_T:    /* ctrl-t, swaps current character with previous. */
        /* Swap the two bytes around the cursor. */
        if (l->pos > 0 && l->pos < l->len) {
            char tmp = l->buf[l->pos];
            l->buf[l->pos] = l->buf[l->pos - 1];
            l->buf[l->pos - 1] = tmp;
            if (l->pos < l->len) l->pos++;
            refresh_line(l);
        }
        break;
    case CTRL_B:     /* ctrl-b */
        pua_lineedit_edit_move_left(l);
        break;
    case CTRL_F:     /* ctrl-f */
        pua_lineedit_edit_move_right(l);
        break;
    case CTRL_P:    /* ctrl-p */
        pua_lineedit_edit_history_next(l, PUA_LINEEDIT_HISTORY_PREV);
        break;
    case CTRL_N:    /* ctrl-n */
        pua_lineedit_edit_history_next(l, PUA_LINEEDIT_HISTORY_NEXT);
        break;
    case ESC:    /* escape sequence */
        /* Handle common arrow-key escape sequences. */
        if (read(l->ifd,seq,1) == -1) break;
        if (read(l->ifd,seq+1,1) == -1) break;

        /* ESC [ sequences. */
        if (seq[0] == '[') {
            switch(seq[1]) {
            case 'A': /* Up */
                pua_lineedit_edit_history_next(l, PUA_LINEEDIT_HISTORY_PREV);
                break;
            case 'B': /* Down */
                pua_lineedit_edit_history_next(l, PUA_LINEEDIT_HISTORY_NEXT);
                break;
            case 'C': /* Right */
                pua_lineedit_edit_move_right(l);
                break;
            case 'D': /* Left */
                pua_lineedit_edit_move_left(l);
                break;
            }
        }
        break;
    default:
        if (pua_lineedit_edit_insert(l, &c, 1)) return NULL;
        break;
    case CTRL_U: /* Ctrl+u, delete the whole line. */
        l->buf[0] = '\0';
        l->pos = l->len = 0;
        refresh_line(l);
        break;
    case CTRL_K: /* Ctrl+k, delete from current to end of line. */
        l->buf[l->pos] = '\0';
        l->len = l->pos;
        refresh_line(l);
        break;
    case CTRL_A: /* Ctrl+a, go to the start of the line */
        pua_lineedit_edit_move_home(l);
        break;
    case CTRL_E: /* ctrl+e, go to the end of the line */
        pua_lineedit_edit_move_end(l);
        break;
    case CTRL_L: /* ctrl+l, clear screen */
        clear_screen();
        refresh_line(l);
        break;
    case CTRL_W: /* ctrl+w, delete previous word */
        pua_lineedit_edit_delete_prev_word(l);
        break;
    }
    return pua_lineedit_edit_more;
}

/* Internal helper called after pua_lineedit_edit_feed() returns a final value.
 * At this point the user input is in the buffer and terminal mode can be
 * restored. */
static void pua_lineedit_edit_stop(struct pua_lineedit_state *l) {
    if (!isatty(l->ifd) && !getenv("LINENOISE_ASSUME_TTY")) return;
    disable_raw_mode(l->ifd);
    printf("\n");
}

/* Implements the blocking line-edit loop used by pua_lineedit(). */
static char *pua_lineedit_blocking_edit(int stdin_fd, int stdout_fd, char *buf, size_t buflen, const char *prompt)
{
    struct pua_lineedit_state l;

    /* Editing without a buffer is invalid. */
    if (buflen == 0) {
        errno = EINVAL;
        return NULL;
    }

    pua_lineedit_edit_start(&l,stdin_fd,stdout_fd,buf,buflen,prompt);
    char *res;
    while((res = pua_lineedit_edit_feed(&l)) == pua_lineedit_edit_more);
    pua_lineedit_edit_stop(&l);
    return res;
}

/* This function is called when pua_lineedit() is called with the standard
 * input file descriptor not attached to a TTY. So for example when the
 * program using pua_lineedit is called in pipe or with a file redirected
 * to its standard input. In this case, we want to be able to return the
 * line regardless of its length (by default we are limited to 4k). */
static char *pua_lineedit_no_tty(void) {
    char *line = NULL;
    size_t len = 0, maxlen = 0;

    while(1) {
        if (len == maxlen) {
            if (maxlen == 0) maxlen = 16;
            maxlen *= 2;
            char *oldval = line;
            line = realloc(line,maxlen);
            if (line == NULL) {
                if (oldval) free(oldval);
                return NULL;
            }
        }
        int c = fgetc(stdin);
        if (c == EOF || c == '\n') {
            if (c == EOF && len == 0) {
                free(line);
                return NULL;
            } else {
                line[len] = '\0';
                return line;
            }
        } else {
            line[len] = c;
            len++;
        }
    }
}

/* The high level function that is the main API of the pua_lineedit library.
 * This function checks if the terminal has basic capabilities, just checking
 * for a blacklist of stupid terminals, and later either calls the line
 * editing function or uses dummy fgets() so that you will be able to type
 * something even in the most desperate of the conditions. */
char *pua_lineedit(const char *prompt) {
    char buf[PUA_LINEEDIT_MAX_LINE];

    if (!isatty(STDIN_FILENO) && !getenv("LINENOISE_ASSUME_TTY")) {
        /* Not a tty: read from file / pipe. In this mode we don't want any
         * limit to the line size, so we call a function to handle that. */
        return pua_lineedit_no_tty();
    } else if (is_unsupported_term()) {
        size_t len;

        printf("%s",prompt);
        fflush(stdout);
        if (fgets(buf,PUA_LINEEDIT_MAX_LINE,stdin) == NULL) return NULL;
        len = strlen(buf);
        while(len && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
            len--;
            buf[len] = '\0';
        }
        return strdup(buf);
    } else {
        char *retval = pua_lineedit_blocking_edit(STDIN_FILENO,STDOUT_FILENO,buf,PUA_LINEEDIT_MAX_LINE,prompt);
        return retval;
    }
}

/* ================================ History ================================= */

/* Free the history, but does not reset it. Only used when we have to
 * exit() to avoid memory leaks are reported by valgrind & co. */
static void free_history(void) {
    if (history) {
        int j;

        for (j = 0; j < history_len; j++)
            free(history[j]);
        free(history);
    }
}

/* At exit we'll try to fix the terminal to the initial conditions. */
static void pua_lineedit_at_exit(void) {
    disable_raw_mode(STDIN_FILENO);
    free_history();
}

/* This is the API call to add a new entry in the pua_lineedit history.
 * It uses a fixed array of char pointers that are shifted (memmoved)
 * when the history max length is reached in order to remove the older
 * entry and make room for the new one, so it is not exactly suitable for huge
 * histories, but will work well for a few hundred of entries.
 *
 * Using a circular buffer is smarter, but a bit more complex to handle. */
int pua_lineedit_history_add(const char *line) {
    char *linecopy;

    if (history_max_len == 0) return 0;

    /* Initialization on first call. */
    if (history == NULL) {
        history = malloc(sizeof(char*)*history_max_len);
        if (history == NULL) return 0;
        memset(history,0,(sizeof(char*)*history_max_len));
    }

    /* Don't add duplicated lines. */
    if (history_len && !strcmp(history[history_len-1], line)) return 0;

    /* Add an heap allocated copy of the line in the history.
     * If we reached the max length, remove the older line. */
    linecopy = strdup(line);
    if (!linecopy) return 0;
    if (history_len == history_max_len) {
        free(history[0]);
        memmove(history,history+1,sizeof(char*)*(history_max_len-1));
        history_len--;
    }
    history[history_len] = linecopy;
    history_len++;
    return 1;
}

/* Set the maximum length for the history. This function can be called even
 * if there is already some history, the function will make sure to retain
 * just the latest 'len' elements if the new history length value is smaller
 * than the amount of items already inside the history. */
int pua_lineedit_history_set_max_len(int len) {
    char **new;

    if (len < 1) return 0;
    if (history) {
        int tocopy = history_len;

        new = malloc(sizeof(char*)*len);
        if (new == NULL) return 0;

        /* If we can't copy everything, free the elements we'll not use. */
        if (len < tocopy) {
            int j;

            for (j = 0; j < tocopy-len; j++) free(history[j]);
            tocopy = len;
        }
        memset(new,0,sizeof(char*)*len);
        memcpy(new,history+(history_len-tocopy), sizeof(char*)*tocopy);
        free(history);
        history = new;
    }
    history_max_len = len;
    if (history_len > history_max_len)
        history_len = history_max_len;
    return 1;
}
