#include <stdlib.h>

#include "internal.h"
#include "fstring.h"

void fstring(int canAssign) {
    (void)canAssign;
    int baseTop = typeStackTop;

    // Token is f"..." - extract content (skip f" at start and " at end)
    const char* src = parser.previous.start + 2;
    int rawLen = parser.previous.length - 3;

    int partCount = 0;
    int i = 0;

    while (i < rawLen) {
        // Find next { or end of string
        int start = i;
        while (i < rawLen && src[i] != '{') {
            if (src[i] == '\\' && i + 1 < rawLen) i++; // skip escape
            i++;
        }

        // Emit literal part (start to i)
        if (i > start) {
            // Process escape sequences in literal part
            char* buf = (char*)malloc(i - start + 1);
            int w = 0;
            for (int j = start; j < i; j++) {
                char c = src[j];
                if (c == '\\' && j + 1 < i) {
                    char e = src[++j];
                    switch (e) {
                        case 'n': buf[w++] = '\n'; break;
                        case 't': buf[w++] = '\t'; break;
                        case 'r': buf[w++] = '\r'; break;
                        case '"': buf[w++] = '"'; break;
                        case '\\': buf[w++] = '\\'; break;
                        case '{': buf[w++] = '{'; break;
                        case '}': buf[w++] = '}'; break;
                        default:
                            buf[w++] = '\\';
                            buf[w++] = e;
                            break;
                    }
                } else {
                    buf[w++] = c;
                }
            }
            ObjString* s = copyString(buf, w);
            free(buf);
            emitConstant(OBJ_VAL(s));
            partCount++;
        }

        if (i < rawLen && src[i] == '{') {
            i++; // skip {

            // Find matching }
            int exprStart = i;
            int braceDepth = 1;
            while (i < rawLen && braceDepth > 0) {
                // Check for multiline strings [[...]]
                if (src[i] == '[' && i + 1 < rawLen && src[i+1] == '[') {
                    i += 2;
                    while (i < rawLen && !(src[i] == ']' && i + 1 < rawLen && src[i+1] == ']')) {
                        i++;
                    }
                    if (i < rawLen) i += 2; // consume ]]
                    continue;
                }

                // Check for comments --...
                if (src[i] == '-' && i + 1 < rawLen && src[i+1] == '-') {
                    i += 2;
                    while (i < rawLen && src[i] != '\n') i++;
                    continue;
                }

                // Check for string start: \"
                if (src[i] == '\\' && i + 1 < rawLen && src[i+1] == '"') {
                    i += 2; // enter string
                    while (i < rawLen) {
                        // Check for escaped backslash (double backslash)
                        if (src[i] == '\\' && i + 1 < rawLen && src[i+1] == '\\') {
                            i += 2;
                            continue;
                        }

                        // Check for closing quote \"
                        if (src[i] == '\\' && i + 1 < rawLen && src[i+1] == '"') {
                            i += 2;
                            break;
                        }
                        i++;
                    }
                    continue;
                }

                // Check for other escapes (skip them so we don't count \{ or \})
                if (src[i] == '\\') {
                    if (i + 1 < rawLen) i += 2;
                    else i++;
                    continue;
                }

                if (src[i] == '{') braceDepth++;
                else if (src[i] == '}') braceDepth--;
                if (braceDepth > 0) i++;
            }
            int exprLen = i - exprStart;
            i++; // skip closing }

            if (exprLen > 0) {
                if (emitSimpleFstringExpr(src + exprStart, exprLen)) {
                    partCount++;
                    continue;
                }

                // Save current parser/lexer state
                Parser savedParser = parser;
                Lexer savedLexer = lexer;

                // Create expression source for standalone parsing.
                // We must unescape \" -> " and \\ -> \ etc.
                char* exprSrc = (char*)malloc(exprLen + 10);
                int w = 0;
                for (int j = exprStart; j < exprStart + exprLen; j++) {
                    if (src[j] == '\\' && j + 1 < exprStart + exprLen) {
                        if (src[j+1] == '"') {
                            exprSrc[w++] = '"';
                            j++;
                        } else if (src[j+1] == '\\') {
                            exprSrc[w++] = '\\';
                            j++;
                        } else if (src[j+1] == '{' || src[j+1] == '}') {
                            exprSrc[w++] = src[j+1];
                            j++;
                        } else {
                            // Keep other escapes as is (e.g. \n)
                            exprSrc[w++] = '\\';
                        }
                    } else {
                        exprSrc[w++] = src[j];
                    }
                }
                exprSrc[w] = '\0';

                // Initialize new lexer for the expression
                initLexer(&lexer, exprSrc);

                // Reset parser for expression
                parser.hadError = 0;
                parser.panicMode = 0;
                advance();

                // Compile the raw expression; conversion to string happens in OP_BUILD_STRING.
                expression();

                free(exprSrc);

                // Restore parser/lexer state
                parser = savedParser;
                lexer = savedLexer;

                partCount++;
            }
        }
    }

    // Handle empty f-string
    if (partCount == 0) {
        ObjString* s = copyString("", 0);
        emitConstant(OBJ_VAL(s));
        typeStackTop = baseTop;
        typePush(TYPEHINT_STR);
        return;
    }

    if (partCount > 255) {
        error("f-string has too many parts.");
        typeStackTop = baseTop;
        typePush(TYPEHINT_STR);
        return;
    }

    emitBytes(OP_BUILD_STRING, (uint8_t)partCount);
    typeStackTop = baseTop;
    typePush(TYPEHINT_STR);
}
