/* pua_lineedit.h -- VERSION 1.0
 *
 * Guerrilla line editing library against the idea that a line editing lib
 * needs to be 20,000 lines of C code.
 *
 * See pua_lineedit.c for more information.
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
 */

#ifndef __PUA_LINEEDIT_H
#define __PUA_LINEEDIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h> /* For size_t. */

typedef struct pua_lineedit_completions {
  size_t len;
  char **cvec;
} pua_lineedit_completions;

/* Blocking API. */
char *pua_lineedit(const char *prompt);

/* Completion API. */
typedef void(pua_lineedit_completion_callback)(const char *, pua_lineedit_completions *);
typedef void(pua_lineedit_syntax_highlight_callback)(const char *buf, char *highlighted, size_t maxlen);
void pua_lineedit_set_completion_callback(pua_lineedit_completion_callback *);
void pua_lineedit_set_syntax_highlight_callback(pua_lineedit_syntax_highlight_callback *);
void pua_lineedit_add_completion(pua_lineedit_completions *, const char *);

/* History API. */
int pua_lineedit_history_add(const char *line);
int pua_lineedit_history_set_max_len(int len);
void pua_lineedit_set_multi_line(int ml);

#ifdef __cplusplus
}
#endif

#endif /* __PUA_LINEEDIT_H */
