package dev.pua.idea

import com.intellij.lexer.Lexer
import com.intellij.openapi.editor.DefaultLanguageHighlighterColors
import com.intellij.openapi.editor.HighlighterColors
import com.intellij.openapi.editor.colors.TextAttributesKey
import com.intellij.openapi.fileTypes.SyntaxHighlighterBase
import com.intellij.psi.tree.IElementType

class PuaSyntaxHighlighter : SyntaxHighlighterBase() {
    override fun getHighlightingLexer(): Lexer = PuaLexer()

    override fun getTokenHighlights(tokenType: IElementType): Array<TextAttributesKey> = when (tokenType) {
        PuaTypes.KEYWORD -> pack(KEYWORD)
        PuaTypes.IDENTIFIER -> pack(IDENTIFIER)
        PuaTypes.STRING -> pack(STRING)
        PuaTypes.NUMBER -> pack(NUMBER)
        PuaTypes.COMMENT -> pack(COMMENT)
        PuaTypes.OPERATOR -> pack(OPERATOR)
        PuaTypes.LPAREN, PuaTypes.RPAREN -> pack(PAREN)
        PuaTypes.LBRACE, PuaTypes.RBRACE -> pack(BRACES)
        PuaTypes.LBRACKET, PuaTypes.RBRACKET -> pack(BRACKETS)
        PuaTypes.COMMA, PuaTypes.DOT, PuaTypes.SEMICOLON, PuaTypes.COLON -> pack(PUNCT)
        PuaTypes.QUESTION -> pack(OPERATOR)
        PuaTypes.BAD_CHARACTER -> pack(BAD_CHAR)
        else -> emptyArray()
    }

    companion object {
        val KEYWORD: TextAttributesKey = TextAttributesKey.createTextAttributesKey(
            "PUA_KEYWORD",
            DefaultLanguageHighlighterColors.KEYWORD,
        )
        val IDENTIFIER: TextAttributesKey = TextAttributesKey.createTextAttributesKey(
            "PUA_IDENTIFIER",
            DefaultLanguageHighlighterColors.IDENTIFIER,
        )
        val STRING: TextAttributesKey = TextAttributesKey.createTextAttributesKey(
            "PUA_STRING",
            DefaultLanguageHighlighterColors.STRING,
        )
        val NUMBER: TextAttributesKey = TextAttributesKey.createTextAttributesKey(
            "PUA_NUMBER",
            DefaultLanguageHighlighterColors.NUMBER,
        )
        val COMMENT: TextAttributesKey = TextAttributesKey.createTextAttributesKey(
            "PUA_COMMENT",
            DefaultLanguageHighlighterColors.LINE_COMMENT,
        )
        val OPERATOR: TextAttributesKey = TextAttributesKey.createTextAttributesKey(
            "PUA_OPERATOR",
            DefaultLanguageHighlighterColors.OPERATION_SIGN,
        )
        val PAREN: TextAttributesKey = TextAttributesKey.createTextAttributesKey(
            "PUA_PAREN",
            DefaultLanguageHighlighterColors.PARENTHESES,
        )
        val BRACES: TextAttributesKey = TextAttributesKey.createTextAttributesKey(
            "PUA_BRACES",
            DefaultLanguageHighlighterColors.BRACES,
        )
        val BRACKETS: TextAttributesKey = TextAttributesKey.createTextAttributesKey(
            "PUA_BRACKETS",
            DefaultLanguageHighlighterColors.BRACKETS,
        )
        val PUNCT: TextAttributesKey = TextAttributesKey.createTextAttributesKey(
            "PUA_PUNCT",
            DefaultLanguageHighlighterColors.COMMA,
        )
        val BAD_CHAR: TextAttributesKey = TextAttributesKey.createTextAttributesKey(
            "PUA_BAD_CHAR",
            HighlighterColors.BAD_CHARACTER,
        )
    }
}
