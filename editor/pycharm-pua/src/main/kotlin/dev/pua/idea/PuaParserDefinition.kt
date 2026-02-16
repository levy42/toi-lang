package dev.pua.idea

import com.intellij.lang.ASTNode
import com.intellij.lang.ParserDefinition
import com.intellij.lang.PsiParser
import com.intellij.lexer.Lexer
import com.intellij.openapi.project.Project
import com.intellij.psi.FileViewProvider
import com.intellij.psi.PsiElement
import com.intellij.psi.PsiFile
import com.intellij.psi.TokenType
import com.intellij.psi.impl.source.tree.ASTWrapperPsiElement
import com.intellij.psi.tree.IFileElementType
import com.intellij.psi.tree.TokenSet

class PuaParserDefinition : ParserDefinition {
    override fun createLexer(project: Project?): Lexer = PuaLexer()

    override fun createParser(project: Project?): PsiParser = PuaParser()

    override fun getFileNodeType(): IFileElementType = FILE

    override fun getCommentTokens(): TokenSet = COMMENTS

    override fun getStringLiteralElements(): TokenSet = STRINGS

    override fun createElement(node: ASTNode): PsiElement = ASTWrapperPsiElement(node)

    override fun createFile(viewProvider: FileViewProvider): PsiFile = PuaFile(viewProvider)

    override fun spaceExistenceTypeBetweenTokens(left: ASTNode, right: ASTNode): ParserDefinition.SpaceRequirements {
        if (left.elementType == TokenType.WHITE_SPACE || right.elementType == TokenType.WHITE_SPACE) {
            return ParserDefinition.SpaceRequirements.MAY
        }
        return ParserDefinition.SpaceRequirements.MAY
    }

    companion object {
        val FILE = IFileElementType(PuaLanguage)
        private val COMMENTS = TokenSet.create(PuaTypes.COMMENT)
        private val STRINGS = TokenSet.create(PuaTypes.STRING)
    }
}
