package dev.pua.idea

import com.intellij.formatting.AbstractBlock
import com.intellij.formatting.Alignment
import com.intellij.formatting.Block
import com.intellij.formatting.ChildAttributes
import com.intellij.formatting.FormattingContext
import com.intellij.formatting.FormattingModel
import com.intellij.formatting.FormattingModelBuilder
import com.intellij.formatting.FormattingModelProvider
import com.intellij.formatting.Indent
import com.intellij.formatting.Spacing
import com.intellij.formatting.SpacingBuilder
import com.intellij.formatting.Wrap
import com.intellij.lang.ASTNode
import com.intellij.psi.TokenType

class PuaFormattingModelBuilder : FormattingModelBuilder {
    override fun createModel(formattingContext: FormattingContext): FormattingModel {
        val spacingBuilder = createSpacingBuilder(formattingContext)
        val rootBlock = PuaBlock(formattingContext.node, null, null, spacingBuilder)
        return FormattingModelProvider.createFormattingModelForPsiFile(
            formattingContext.containingFile,
            rootBlock,
            formattingContext.codeStyleSettings,
        )
    }

    private fun createSpacingBuilder(context: FormattingContext): SpacingBuilder {
        return SpacingBuilder(context.codeStyleSettings, PuaLanguage)
            .around(PuaTypes.OPERATOR).spaces(1)
            .after(PuaTypes.COMMA).spaceIf(true)
            .before(PuaTypes.COMMA).spaceIf(false)
            .before(PuaTypes.COLON).spaceIf(false)
            .after(PuaTypes.COLON).spaceIf(true)
            .before(PuaTypes.QUESTION).spaceIf(true)
            .after(PuaTypes.QUESTION).spaceIf(true)
    }
}

private class PuaBlock(
    node: ASTNode,
    wrap: Wrap?,
    alignment: Alignment?,
    private val spacingBuilder: SpacingBuilder,
) : AbstractBlock(node, wrap, alignment) {

    override fun buildChildren(): List<Block> {
        val blocks = ArrayList<Block>()
        var child = myNode.firstChildNode
        while (child != null) {
            if (child.elementType != TokenType.WHITE_SPACE) {
                blocks.add(PuaBlock(child, Wrap.createWrap(Wrap.Type.NONE, false), null, spacingBuilder))
            }
            child = child.treeNext
        }
        return blocks
    }

    override fun getIndent(): Indent? = Indent.getNoneIndent()

    override fun getSpacing(child1: Block?, child2: Block): Spacing? = spacingBuilder.getSpacing(this, child1, child2)

    override fun getChildAttributes(newChildIndex: Int): ChildAttributes = ChildAttributes(Indent.getNoneIndent(), null)

    override fun isLeaf(): Boolean = myNode.firstChildNode == null
}
