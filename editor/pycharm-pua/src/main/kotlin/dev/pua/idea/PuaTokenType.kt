package dev.pua.idea

import com.intellij.psi.tree.IElementType

class PuaTokenType(debugName: String) : IElementType(debugName, PuaLanguage) {
    override fun toString(): String = "PuaTokenType." + super.toString()
}
