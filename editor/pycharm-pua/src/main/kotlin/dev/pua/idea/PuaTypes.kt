package dev.pua.idea

import com.intellij.psi.TokenType
import com.intellij.psi.tree.IElementType

object PuaTypes {
    val WHITE_SPACE: IElementType = TokenType.WHITE_SPACE
    val BAD_CHARACTER: IElementType = TokenType.BAD_CHARACTER

    val COMMENT = PuaTokenType("COMMENT")
    val IDENTIFIER = PuaTokenType("IDENTIFIER")
    val KEYWORD = PuaTokenType("KEYWORD")
    val NUMBER = PuaTokenType("NUMBER")
    val STRING = PuaTokenType("STRING")
    val OPERATOR = PuaTokenType("OPERATOR")

    val LPAREN = PuaTokenType("LPAREN")
    val RPAREN = PuaTokenType("RPAREN")
    val LBRACE = PuaTokenType("LBRACE")
    val RBRACE = PuaTokenType("RBRACE")
    val LBRACKET = PuaTokenType("LBRACKET")
    val RBRACKET = PuaTokenType("RBRACKET")
    val COMMA = PuaTokenType("COMMA")
    val DOT = PuaTokenType("DOT")
    val SEMICOLON = PuaTokenType("SEMICOLON")
    val COLON = PuaTokenType("COLON")
    val QUESTION = PuaTokenType("QUESTION")
}
