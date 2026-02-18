#include <stdbool.h>
#include <stdint.h>
#include <tree_sitter/parser.h>
#include "tree_sitter/array.h"

enum TokenType {
  NEWLINE,
  INDENT,
  DEDENT,
};

typedef struct {
  Array(uint16_t) indent_stack;
  uint16_t pending_dedents;
  bool at_line_start;
} Scanner;

static void scanner_init(Scanner *scanner) {
  array_init(&scanner->indent_stack);
  array_push(&scanner->indent_stack, 0);
  scanner->pending_dedents = 0;
  scanner->at_line_start = true;
}

static void scanner_advance(TSLexer *lexer) { lexer->advance(lexer, false); }
static void scanner_skip(TSLexer *lexer) { lexer->advance(lexer, true); }

void *tree_sitter_pua_external_scanner_create(void) {
  Scanner *scanner = ts_calloc(1, sizeof(Scanner));
  scanner_init(scanner);
  return scanner;
}

void tree_sitter_pua_external_scanner_destroy(void *payload) {
  Scanner *scanner = (Scanner *)payload;
  array_delete(&scanner->indent_stack);
  ts_free(scanner);
}

unsigned tree_sitter_pua_external_scanner_serialize(void *payload, char *buffer) {
  Scanner *scanner = (Scanner *)payload;
  uint32_t size = 0;

  if (size < TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
    buffer[size++] = (char)scanner->at_line_start;
  }
  if (size < TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
    buffer[size++] = (char)scanner->pending_dedents;
  }

  uint32_t count = scanner->indent_stack.size;
  if (count > 255) count = 255;
  if (size < TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
    buffer[size++] = (char)count;
  }

  for (uint32_t i = 0; i < count && size + 1 < TREE_SITTER_SERIALIZATION_BUFFER_SIZE; i++) {
    uint16_t v = *array_get(&scanner->indent_stack, i);
    buffer[size++] = (char)(v & 0xFF);
    buffer[size++] = (char)((v >> 8) & 0xFF);
  }

  return size;
}

void tree_sitter_pua_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  Scanner *scanner = (Scanner *)payload;
  array_clear(&scanner->indent_stack);
  scanner->pending_dedents = 0;
  scanner->at_line_start = true;

  if (length < 3) {
    scanner_init(scanner);
    return;
  }

  uint32_t idx = 0;
  scanner->at_line_start = (bool)buffer[idx++];
  scanner->pending_dedents = (uint8_t)buffer[idx++];
  uint8_t count = (uint8_t)buffer[idx++];

  for (uint8_t i = 0; i < count; i++) {
    if (idx + 1 >= length) break;
    uint16_t v = (uint8_t)buffer[idx++] | ((uint16_t)(uint8_t)buffer[idx++] << 8);
    array_push(&scanner->indent_stack, v);
  }

  if (scanner->indent_stack.size == 0) {
    array_push(&scanner->indent_stack, 0);
  }
}

bool tree_sitter_pua_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  Scanner *scanner = (Scanner *)payload;

  if (lexer->lookahead == '\r') {
    scanner_advance(lexer);
  }

  if (scanner->pending_dedents > 0 && valid_symbols[DEDENT]) {
    scanner->pending_dedents--;
    scanner->at_line_start = false;
    lexer->result_symbol = DEDENT;
    return true;
  }

  if (lexer->lookahead == 0) {
    if (scanner->indent_stack.size > 1 && valid_symbols[DEDENT]) {
      array_pop(&scanner->indent_stack);
      lexer->result_symbol = DEDENT;
      return true;
    }
    return false;
  }

  if (lexer->lookahead == '\n' && valid_symbols[NEWLINE]) {
    scanner_advance(lexer);
    scanner->at_line_start = true;
    lexer->result_symbol = NEWLINE;
    return true;
  }

  if (scanner->at_line_start && (valid_symbols[INDENT] || valid_symbols[DEDENT] || valid_symbols[NEWLINE])) {
    uint16_t indent = 0;

    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
      if (lexer->lookahead == ' ') indent += 1;
      if (lexer->lookahead == '\t') indent += 4;
      scanner_skip(lexer);
    }

    if (lexer->lookahead == '\n' && valid_symbols[NEWLINE]) {
      scanner_advance(lexer);
      scanner->at_line_start = true;
      lexer->result_symbol = NEWLINE;
      return true;
    }

    if (lexer->lookahead == '-' && valid_symbols[NEWLINE]) {
      scanner_advance(lexer);
      if (lexer->lookahead == '-') {
        while (lexer->lookahead != 0 && lexer->lookahead != '\n') {
          scanner_advance(lexer);
        }
        if (lexer->lookahead == '\n') scanner_advance(lexer);
        scanner->at_line_start = true;
        lexer->result_symbol = NEWLINE;
        return true;
      }
      return false;
    }

    uint16_t current = *array_back(&scanner->indent_stack);

    if (indent > current && valid_symbols[INDENT]) {
      array_push(&scanner->indent_stack, indent);
      scanner->at_line_start = false;
      lexer->result_symbol = INDENT;
      return true;
    }

    if (indent < current && valid_symbols[DEDENT]) {
      while (scanner->indent_stack.size > 1 && indent < *array_back(&scanner->indent_stack)) {
        array_pop(&scanner->indent_stack);
        scanner->pending_dedents++;
      }

      if (indent != *array_back(&scanner->indent_stack)) {
        scanner->pending_dedents = 0;
        return false;
      }

      if (scanner->pending_dedents > 0) {
        scanner->pending_dedents--;
        scanner->at_line_start = false;
        lexer->result_symbol = DEDENT;
        return true;
      }
    }

    scanner->at_line_start = false;
  }

  return false;
}
