package dev.pua.idea

import com.intellij.lexer.LexerBase
import com.intellij.psi.tree.IElementType

class PuaLexer : LexerBase() {
    private var buffer: CharSequence = ""
    private var startOffset: Int = 0
    private var endOffset: Int = 0

    private var tokenStart: Int = 0
    private var tokenEnd: Int = 0
    private var tokenType: IElementType? = null

    private val keywords = setOf(
        "and", "or", "not", "nil", "true", "false",
        "print", "local", "global", "fn", "return", "if", "elif", "else",
        "while", "for", "in", "break", "continue", "with", "as",
        "try", "except", "finally", "throw", "has", "import", "gc", "del"
    )

    override fun start(
        buffer: CharSequence,
        startOffset: Int,
        endOffset: Int,
        initialState: Int,
    ) {
        this.buffer = buffer
        this.startOffset = startOffset
        this.endOffset = endOffset
        this.tokenStart = startOffset
        this.tokenEnd = startOffset
        this.tokenType = null
        locateToken()
    }

    override fun getState(): Int = 0

    override fun getTokenType(): IElementType? = tokenType

    override fun getTokenStart(): Int = tokenStart

    override fun getTokenEnd(): Int = tokenEnd

    override fun advance() {
        tokenStart = tokenEnd
        locateToken()
    }

    override fun getBufferSequence(): CharSequence = buffer

    override fun getBufferEnd(): Int = endOffset

    private fun locateToken() {
        if (tokenStart >= endOffset) {
            tokenType = null
            tokenEnd = endOffset
            return
        }

        val c = buffer[tokenStart]

        if (c.isWhitespace()) {
            var i = tokenStart + 1
            while (i < endOffset && buffer[i].isWhitespace()) i++
            tokenEnd = i
            tokenType = PuaTypes.WHITE_SPACE
            return
        }

        if (c == '-' && tokenStart + 1 < endOffset && buffer[tokenStart + 1] == '-') {
            var i = tokenStart + 2
            while (i < endOffset && buffer[i] != '\n') i++
            tokenEnd = i
            tokenType = PuaTypes.COMMENT
            return
        }

        if (c == 'f' && tokenStart + 1 < endOffset && buffer[tokenStart + 1] == '"') {
            tokenEnd = scanQuotedString(tokenStart + 2)
            tokenType = PuaTypes.STRING
            return
        }

        if (c == '"') {
            tokenEnd = scanQuotedString(tokenStart + 1)
            tokenType = PuaTypes.STRING
            return
        }

        if (c == '[' && tokenStart + 1 < endOffset && buffer[tokenStart + 1] == '[') {
            var i = tokenStart + 2
            while (i + 1 < endOffset) {
                if (buffer[i] == ']' && buffer[i + 1] == ']') {
                    i += 2
                    tokenEnd = i
                    tokenType = PuaTypes.STRING
                    return
                }
                i++
            }
            tokenEnd = endOffset
            tokenType = PuaTypes.STRING
            return
        }

        if (c.isDigit()) {
            var i = tokenStart + 1
            while (i < endOffset && (buffer[i].isDigit() || buffer[i] == '_')) i++
            if (i < endOffset && buffer[i] == '.' && i + 1 < endOffset && buffer[i + 1].isDigit()) {
                i++
                while (i < endOffset && (buffer[i].isDigit() || buffer[i] == '_')) i++
            }
            tokenEnd = i
            tokenType = PuaTypes.NUMBER
            return
        }

        if (c.isLetter() || c == '_') {
            var i = tokenStart + 1
            while (i < endOffset && (buffer[i].isLetterOrDigit() || buffer[i] == '_')) i++
            val word = buffer.subSequence(tokenStart, i).toString()
            tokenEnd = i
            tokenType = if (word in keywords) PuaTypes.KEYWORD else PuaTypes.IDENTIFIER
            return
        }

        if (tokenStart + 1 < endOffset) {
            val c2 = buffer[tokenStart + 1]
            if ((c == '.' && c2 == '.') ||
                (c == '/' && c2 == '/') ||
                (c == '*' && c2 == '*') ||
                (c == '=' && c2 == '=') ||
                (c == '!' && c2 == '=') ||
                (c == '<' && c2 == '=') ||
                (c == '>' && c2 == '=')) {
                tokenEnd = tokenStart + 2
                tokenType = PuaTypes.OPERATOR
                return
            }
        }

        when (c) {
            '(' -> single(PuaTypes.LPAREN)
            ')' -> single(PuaTypes.RPAREN)
            '{' -> single(PuaTypes.LBRACE)
            '}' -> single(PuaTypes.RBRACE)
            '[' -> single(PuaTypes.LBRACKET)
            ']' -> single(PuaTypes.RBRACKET)
            ',' -> single(PuaTypes.COMMA)
            ';' -> single(PuaTypes.SEMICOLON)
            ':' -> single(PuaTypes.COLON)
            '?' -> single(PuaTypes.QUESTION)
            '.' -> single(PuaTypes.DOT)
            '+', '-', '*', '/', '%', '#', '=', '<', '>' -> single(PuaTypes.OPERATOR)
            else -> single(PuaTypes.BAD_CHARACTER)
        }
    }

    private fun single(type: IElementType) {
        tokenEnd = tokenStart + 1
        tokenType = type
    }

    private fun scanQuotedString(from: Int): Int {
        var i = from
        while (i < endOffset) {
            val ch = buffer[i]
            if (ch == '\\' && i + 1 < endOffset) {
                i += 2
                continue
            }
            if (ch == '"') return i + 1
            i++
        }
        return endOffset
    }
}
