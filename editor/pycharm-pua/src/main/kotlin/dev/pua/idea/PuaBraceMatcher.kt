package dev.pua.idea

import com.intellij.lang.BracePair
import com.intellij.lang.PairedBraceMatcher
import com.intellij.psi.tree.IElementType

class PuaBraceMatcher : PairedBraceMatcher {
    override fun getPairs(): Array<BracePair> = arrayOf(
        BracePair(PuaTypes.LPAREN, PuaTypes.RPAREN, false),
        BracePair(PuaTypes.LBRACE, PuaTypes.RBRACE, true),
        BracePair(PuaTypes.LBRACKET, PuaTypes.RBRACKET, false),
    )

    override fun isPairedBracesAllowedBeforeType(lbraceType: IElementType, contextType: IElementType?): Boolean = true

    override fun getCodeConstructStart(fileText: CharSequence, openingBraceOffset: Int): Int = openingBraceOffset
}
