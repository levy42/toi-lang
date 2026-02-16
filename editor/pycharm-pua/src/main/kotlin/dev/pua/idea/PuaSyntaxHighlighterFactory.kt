package dev.pua.idea

import com.intellij.openapi.fileTypes.SingleLazyInstanceSyntaxHighlighterFactory
import com.intellij.openapi.fileTypes.SyntaxHighlighter

class PuaSyntaxHighlighterFactory : SingleLazyInstanceSyntaxHighlighterFactory() {
    override fun createHighlighter(): SyntaxHighlighter = PuaSyntaxHighlighter()
}
